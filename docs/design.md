# SpikeDB Design

Reference description of the code in [src/spike_db.c](src/spike_db.c) and
[src/spike_db.h](src/spike_db.h). It documents what the implementation does
today, not a roadmap.

---

## 1. Workload and data model

SpikeDB is a single-file, embeddable store for financial time series
(ticks, candles), with one ingester process and many concurrent reader
processes.

```
Key:    (symbol: uint64, time: uint64, seq: uint32)   — unique, ordered by that triple
Value:  opaque bytes, 0 .. 65000 B
```

`symbol` is opaque to the engine; it selects which per-symbol index to use.
`time` is an arbitrary monotonic-ish integer (nanoseconds in practice).
`seq` breaks ties between events that share a timestamp, which is normal in
market data (one aggressive order prints several fills under a single
exchange nanosecond). Series that are naturally unique per timestamp use
`seq = 0` and the seq-less API wrappers. Duplicate
`(symbol, time, seq)` insertion is rejected with `SPIKEDB_INVAL`.

> Format history: v6 added `seq` to the key, widening `LeafSlot` from 16 to
> 20 bytes and `SkipNode` from 144 to 148. There is no migration path from
> v5 files; `SPIKEDB_MAGIC` was bumped to `"SPIKEDB\x06"`.

Design consequences of the workload:

| Workload property | Design response |
|---|---|
| Append-dominant, occasionally out of order | Fast-append path plus in-leaf sorted insert |
| Range scan `[t0, t1]` per symbol is the dominant read | One skip-list descent, then a doubly-linked leaf walk |
| "What is the newest tick?" polled constantly | `min_time` / `max_time` / `record_count` cached in a per-symbol root page |
| Files far larger than RAM | No `mmap`; explicit page cache over `pread` / `ReadFile` |
| One writer, many reader processes | OS file range lock, shared for readers, exclusive for writers |
| Crash safety without WAL cost | Copy-on-write pages + double-buffered CRC-checked meta pages (see §5 for where this is still incomplete) |
| Retention (drop old data) | `spike_db_truncate_before` frees whole leaves |

Explicit non-goals: secondary indexes, variable-length keys, cross-batch
transactions, MVCC snapshot isolation, compression, big-endian hosts, and
thread safety within a single `SpikeDB*` handle.

---

## 2. File layout

Every page is 64 KB (`SPIKEDB_PAGE_SIZE`).

| Pages | Contents |
|---|---|
| 0–1 | Meta page A / meta page B (double-buffered, CRC-32C) |
| 2–17 | Symbol directory (`SPIKEDB_SYMDIR_PAGES = 16`) |
| 18+ | `SymbolRoot` pages, skip-list node pages, leaf pages, freelist pages |

Pages 18 and above are allocated dynamically: the freelist is consulted
first, then a bump watermark (`total_pages_allocated`) extends the file.
There is no fixed ordering of page kinds beyond page 18 — a page's type is
implied by whatever references it.

The on-disk format is little-endian only, and all on-disk structs use
`#pragma pack(push, 1)`.

---

## 3. On-disk structures

### 3.1 Meta page

```c
struct MetaPage {
    uint64_t magic;                    /* "SPIKEDB\x06" */
    uint64_t txn_id;                   /* monotonic; higher valid one wins */
    uint64_t total_pages_allocated;    /* bump watermark */
    uint32_t freelist_head;            /* page id, or INVALID */
    uint32_t symbol_count;
    uint64_t reader_epoch;             /* bumped on each commit */
    uint32_t user_meta_len;
    uint8_t  user_meta[4096];          /* [u16 klen][u16 vlen][key][val]... */
    uint8_t  reserved[...];
    uint32_t crc32;                    /* CRC-32C over the preceding bytes */
};
```

Meta pages are never held in the page cache; they are read and written
directly through the I/O helpers into `db->meta_buf[2]`.

`user_meta` holds the caller's ingest cursor (see
`spike_db_batch_put_meta`). Putting it in the meta page rather than in a
record is what makes it commit and roll back with the data in the same
transaction, which is what an ingester needs to restart without a gap or
a double-apply. It costs no extra page and no extra fsync. Rollback is
free as well: `txn_rollback` re-reads the meta page from disk.

The checksum is CRC-32C (Castagnoli), computed with SSE4.2 `_mm_crc32_*`
when CPUID reports support and with a table-driven software fallback
otherwise. The polynomial is internal to SpikeDB but must stay stable
across both paths.

### 3.1a Page checksum trailer

**Every** page reserves its last 4 bytes for a CRC-32C of the bytes before
it; the meta page's own `crc32` field already sat there, so data pages
just adopt the same convention. `page_write_checked` stamps it and
`page_read_checked` verifies it, and every data page goes in and out
through exactly those two helpers, so there is one place that could forget
and it is covered by every test that reads anything back.

A read that fails verification returns `SPIKEDB_CORRUPT` from `page_pin`
rather than serving the bytes. That matters more here than in a general
store: a silently torn leaf does not crash anything, it just returns a
wrong price into a model or an order.

The layouts that previously ran to the end of a page were trimmed to fit:
the leaf value heap now grows down from `SPDB_PAGE_BODY` instead of
`SPIKEDB_PAGE_SIZE`, the free list holds one fewer id, and the symbol
directory holds 4095 slots per page instead of 4096 (65520 total).

Cost is one CRC pass per page write and per cache miss. On the 100k-record
benchmark that is ~130 page writes, which does not move ingest throughput;
on a cold scan it is a few percent of the read it accompanies.

### 3.2 Symbol directory (pages 2–17)

An open-addressed hash table of 16-byte slots:

```c
struct SymDirSlot { uint64_t symbol; uint32_t root_page; uint32_t _pad; };
```

16 pages × 4096 slots = 65536 slots total. The home slot is
`mix64(symbol) % 65536`; collisions probe linearly, wrapping across page
boundaries and back to page 2. A slot is empty when both `symbol` and
`root_page` are zero. On the first write for a symbol, a `SymbolRoot` page
is allocated and installed here, and `symbol_count` is bumped in the meta
page; the root is seeded with `rng_state = mix64(symbol ^ 0xC0FFEE)` and
`current_max_level = 1`.

The directory is fixed-size and never grows: a symbol count approaching
65536 degrades probe length, and a full directory returns `SPIKEDB_FULL`.

`spike_db_symbol_drop` leaves a **tombstone** (`root_page = INVALID` with
the symbol id retained) rather than zeroing the slot, because zeroing it
would cut the probe chain of any symbol that collided with it. Lookups
probe past tombstones and creation reuses the first one it saw, so
repeated drop/recreate cycles do not leak slots.

### 3.3 SymbolRoot page (one per symbol)

```c
struct SymbolRootPage {
    uint64_t symbol;
    uint64_t min_time;              /* UINT64_MAX when empty */
    uint64_t max_time;              /* 0 when empty */
    uint64_t record_count;
    uint64_t leaf_count;
    uint32_t first_leaf, last_leaf; /* INVALID when empty */
    uint8_t  current_max_level;     /* tallest in-use skip-list level */
    uint32_t rng_state;             /* per-symbol LCG for tower heights */
    uint32_t current_node_page;     /* node page currently being packed */
    uint64_t head_forward[16];      /* the skip list's head sentinel */
};
```

This page is the entry point for every operation on a symbol. Because the
head sentinel's `forward[]` array lives inline, growing the skip list's
height never relocates the head.

`spike_db_max_time`, `spike_db_min_time` and `spike_db_count` are a single
read of one field of this page (`read_root_field`), so a polling bot pays
one warm cache hit per call.

### 3.4 Skip-list node and node pages

```c
struct SkipNode {              /* 148 bytes */
    uint64_t first_time;       /* first key of the leaf it points at */
    uint32_t first_seq;
    uint32_t leaf_page;
    uint8_t  level;            /* tower height, 1..16 */
    uint64_t forward[16];      /* node refs; 0 = nil */
};
```

Nodes are packed into node pages (16-byte header, 442 nodes per 64 KB
page) rather than burning a page per tower. A node is addressed by a
64-bit *node ref*: `page_id << 24 | slot`, with 1-based slots so that ref
`0` can mean nil. `SymbolRoot.current_node_page` is the page currently
being filled; slots are handed out by bumping `used_count`, and a new page
is allocated when it fills. Individual node slots are never recycled.
Node pages are chained through `next_node_page` from the current one, so
`spike_db_symbol_drop` can enumerate and free a symbol's whole index.

`SPIKEDB_MAX_LEVEL = 16` with `p = 0.5` indexes on the order of 2^16
leaves per symbol.

### 3.5 Leaf page

A leaf uses one of two layouts, chosen by `LeafHeader.record_size`
(0 = variable). The header is padded to 64 bytes so the fixed layout's
columns stay naturally aligned.

**Variable-width** (`record_size == 0`) — the default, for payloads that
differ in size:

```
┌─ LeafHeader (64 B) ──────────────────────────────────────────────────┐
│ symbol, min_time, max_time, record_count, value_heap_bottom,         │
│ prev_leaf, next_leaf, skiplist_node_ref, record_size                 │
├─ LeafSlot[record_count] — sorted by (time, seq), grows up ───────────┤
│ { uint64 time; uint32 seq; uint32 value_offset;                      │
│   uint16 value_len; uint16 pad }                                     │
│                                                                      │
│                        ─── free space ───                            │
│                                                                      │
├─ value heap — grows down from offset 65536 ──────────────────────────┤
│ packed value bytes                                                   │
└──────────────────────────────────────────────────────────────────────┘
```

Free space is `value_heap_bottom - (64 + 20 * record_count)`. A record
needs `20 + value_len` bytes. Capacity is therefore 779 records for
64-byte values and 1487 for 24-byte values.

**Fixed-width** (`record_size != 0`, set by `spike_db_symbol_define`) —
three columnar arrays of fixed capacity
`cap = (65532 - 64) / (12 + record_size)`:

```
┌─ LeafHeader (64 B) ──────────────────────────────────────────────────┐
├─ times[cap]     — uint64, only record_count entries live ────────────┤
├─ seqs[cap]      — uint32 ────────────────────────────────────────────┤
├─ payloads[cap]  — record_size bytes each ────────────────────────────┤
└──────────────────────────────────────────────────────────────────────┘
```

This costs `12 + record_size` per record instead of `20 + value_len`, so
10.5% more rows fit at 64-byte records and 22.3% more at 24-byte ones — on
a 100 GB file that is tens of GB and proportionally less I/O. More
importantly the payload column is contiguous, so `spike_db_read_range` is
one `memcpy` per leaf straight into the caller's array rather than a copy
per record, and there is no value heap to fragment (`leaf_compact` is a
no-op).

Insert and delete shift three arrays instead of one slot directory, which
is the trade for not storing an offset per record.

`leaf_find` / `leaf_key_at` / `leaf_val_at` hide the difference, so
lookups, iteration, splits, truncation and verify are written once.

Leaves of one symbol form a doubly-linked list in time order via
`prev_leaf` / `next_leaf`, and each leaf carries `skiplist_node_ref`, a
back-pointer to its index node.

### 3.6 Freelist page

A singly-linked chain from `MetaPage.freelist_head`. Each page has a
16-byte header (`next_page`, `count`) followed by up to 16379 page IDs.
`page_free` pushes onto the head page, or turns the freed page itself into
a new head when the current head is full. `page_alloc` pops from the head,
and consumes an emptied head page as the allocation.

---

## 4. Algorithms

### 4.1 Descent

`descend(root_pg, time, seq)` walks the skip list from
`current_max_level - 1` down to level 0, following `forward[level]` while
the next node's `(first_time, first_seq) <= (time, seq)`. It returns:

- `leaf_page` — the rightmost leaf whose first key is `<= (time, seq)`,
  falling back to `first_leaf` when no such node exists (i.e. the key
  precedes all data);
- `pred_ref[level]` — the predecessor node ref at each level, reused
  directly by `skiplist_splice`.

Carrying `first_seq` on the node matters: without it a run of records
sharing one timestamp that spans several leaves would descend to the
last of those leaves and miss the earlier ones.

Within a leaf, `leaf_search` is a scalar binary search over the slot
directory returning the first slot with key `>= target` plus an `exact`
flag — at most about 12 comparisons on a full leaf.

### 4.2 Insert (`symbol_insert`)

Three cases, in order of frequency:

1. **Fast append.** The key sorts after every record in the target leaf,
   the leaf has no room, and it is the last leaf
   (`next_leaf == INVALID`). A fresh leaf is allocated,
   the record is written into it, the chain is linked, the root's
   counters and `last_leaf` are updated, and a new index node is spliced
   in at a random height. No data is moved. This is the dominant ingest
   path.
2. **In-leaf insert.** `leaf_upsert` places the value at
   `value_heap_bottom`, `memmove`s the slot-directory tail by one slot,
   and updates the header. Existing values never move. When the key is
   already present and `SPIKEDB_PUT_OVERWRITE` was requested, the value
   is rewritten in place if it fits in the old slot's span and appended
   to the heap otherwise; the record count and the leaf's time bounds are
   unchanged, and the abandoned bytes are reclaimed by the next
   `leaf_compact`.
3. **Split.** When the target leaf is full and the append path does not
   apply, `leaf_split` moves the upper half of the records (and their
   values) into a new leaf, compacts the surviving half's value heap
   (`leaf_compact`, using a reusable scratch page), splices the new leaf
   into the linked list, and adds an index node at a random height. The
   record is then retried into whichever half now covers its time.

Tower heights come from `random_level`, a per-symbol LCG in
`SymbolRoot.rng_state` yielding `p = 0.5` — the height is stored on disk
so it survives reopen without a global RNG.

`skiplist_splice` links the new node at levels `0 .. level-1` using the
predecessor refs from the descent, raises `current_max_level` if needed,
and writes the leaf's `skiplist_node_ref` back-pointer.

### 4.3 Batch write (`spike_db_write`)

1. Acquire the exclusive file lock; refresh meta.
2. `txn_begin`.
3. `qsort` the batch by `(symbol, time, seq)`, with the submission index
   as the final tiebreaker so entries touching one key apply in the order
   the caller queued them.
4. Walk the sorted entries; because they are grouped by symbol, the
   symbol-directory lookup happens once per distinct symbol. Puts go
   through `symbol_insert` (with `overwrite` set for
   `SPIKEDB_PUT_OVERWRITE` entries), deletes through `symbol_delete`.
   A delete of an absent key, or of a symbol that was never written, is a
   no-op rather than an error — replaying a correction has to be safe.
   A symbol root page is only created for entries that actually insert.
5. Apply any queued `user_meta` updates to the in-memory meta page.
6. On any failure, roll back and return `SPIKEDB_FULL` if the cache ran
   out of unpinned victims, otherwise the originating status (so a
   duplicate key surfaces as `SPIKEDB_INVAL`).
7. Otherwise `txn_commit`, then release the lock.

`spike_db_write` does not clear the batch; reusing it without
`spike_db_batch_clear` will replay the same records and fail as
duplicates.

### 4.4 Point lookup (`lookup_key`)

One helper backs all four public forms. Shared lock → refresh meta →
symbol lookup → descend → `leaf_search`, then:

| `dir` | Public entry point | Slot taken |
|---|---|---|
| `0` | `spike_db_get`, `spike_db_get_seq` | `pos`, only if `exact` |
| `-1` | `spike_db_get_le` (as-of, `seq = UINT32_MAX`) | `pos` if exact, else `pos - 1` |
| `+1` | `spike_db_get_ge` (`seq = 0`) | `pos`, if within the leaf |

When the chosen slot falls outside the landing leaf, the directional
forms walk `prev_leaf` / `next_leaf`. Every key on a neighbouring leaf
lies on one side of the target, so re-running the same search there lands
on the last slot (going backwards) or slot 0 (going forwards); empty
leaves are simply skipped. The value is copied into a `malloc`'d buffer
the caller frees with `spike_db_free`.

`get_le` is the point-in-time primitive — last trade as of `T`,
prevailing quote as of `T` — and is what avoids lookahead bias in feature
pipelines.

### 4.5 Range scan

Positioning (`iter_position`) descends to the leaf covering the target key
and walks forward to the first slot inside `[time_lo, time_hi]`. The range
is inclusive on both ends and covers every `seq` at each timestamp;
records are yielded in `(time, seq)` order. Positioning is `O(log N)`; the
rest of the scan is a linked-list walk over sorted, contiguous leaf
payload.

Records are copied out **a chunk at a time** (`iter_fill`): up to 1024
records or 256 KB, packed into an iterator-owned buffer as
`[u64 time][u32 seq][u32 len][bytes]`. `spike_db_iter_next` then serves
from that buffer with no locking, no pinning and no per-record copy — the
pointer it hands back points straight into the chunk. Total bytes copied
is the same as the old per-record path, but the page is pinned once per
leaf instead of once per record.

`spike_db_iter_next_batch` exposes the chunk directly for replay loops.
A short non-zero return only means the chunk ran out.

#### Isolation

The default mode holds the shared lock for the iterator's lifetime. The
view is stable, but **the writer is blocked until the iterator closes** —
with the workload this store targets (hours-long training scans against a
live ingester) that means an unbounded ingest stall, so it is only
appropriate for short scans.

`SPIKEDB_SCAN_NONBLOCKING` drops the lock between chunks. A writer then
waits at most for one chunk copy. The cost is isolation: reads are
read-committed, not snapshot. On each refill the iterator compares
`txn_id` against the value it last saw, and if the file has moved on it
re-descends from just past the last key it handed out — `(t, seq + 1)`,
or `(t + 1, 0)` when `seq` is saturated, which is exactly "strictly
greater" without needing a second search mode. Consequences, all of them
intentional:

- the cursor only ever moves forward and never repeats a record;
- records committed ahead of the cursor become visible mid-scan;
- records inserted behind it are not revisited;
- a record can be deleted before the cursor reaches it;
- once the range is exhausted the iterator stays finished — it is not a
  live tail.

A true snapshot is a bigger change than it looks. Modified pages are
rewritten **in place** at commit (only pages the transaction allocated are
ever spilled early), so an older reader's version is gone as soon as the
writer flushes. Real MVCC here needs pre-image copies in an undo area
plus epoch-based reclamation and a cross-process reader registry; page
copy-on-write alone does not work, because a skip-list node has many
incoming references (predecessor `forward[]` pointers at every level, plus
`leaf_page` and the leaf's `skiplist_node_ref` back-pointer), so
relocating one page cascades without bound.

#### Direction

`SPIKEDB_SCAN_REVERSE` walks newest to oldest: positioning finds the
greatest key `<= time_hi` (`cursor_seek_rev`, walking `prev_leaf` when the
landing leaf holds nothing at or below the target) and the chunk fill
steps down through each leaf. "Give me the last N ticks" and "walk back to
the session open" are then bounded work instead of a scan from the start
of the range. The non-blocking resume point mirrors accordingly —
`iter_resume_key` steps down instead of up.

`spike_db_iter_seek` repositions a live iterator in its current direction,
reusing its lock and buffer.

Merged scans are forward-only.

#### Merged multi-symbol replay

`spike_db_scan_multi` keeps one small cursor per symbol — `(leaf, slot)`
plus the key of the record it is sitting on — and a binary min-heap of
cursor indices ordered by `(time, seq, symbol)`. Filling a chunk pops the
heap, copies that record, advances the cursor, and sifts down; only the
output chunk is buffered, so memory is `O(symbols)` plus one chunk rather
than one chunk per symbol. Advancing inside a leaf reuses the pin the copy
already took, so the common case is one pin per record.

Caching each cursor's current key on the cursor is what lets the heap be
ordered without pinning anything.

The resume point after a concurrent commit has to be symbol-aware. Several
symbols printing at the same nanosecond is the normal case, not an edge
case, so resuming at `(t, seq + 1)` would silently drop every tie that had
not been emitted yet, and resuming at `(t, seq)` would re-emit the ones
that had. `multi_reposition` therefore seeks every cursor to `(t, seq)`
inclusive and then drops that exact key for symbols at or below the last
one emitted. Because every cursor resumes from the same global point, the
merged stream is guaranteed non-decreasing: records that landed behind the
global cursor are skipped rather than emitted late.

The cost is one re-descent per symbol per commit observed, which is why
non-blocking merges suit watchlists of tens to hundreds rather than
thousands.

### 4.6 Delete (`symbol_delete`, `spike_db_delete_range`)

`symbol_delete` descends to the leaf, `memmove`s the slot directory down
over the removed slot, and then has three cases to repair:

- **Leaf still populated, slot 0 removed.** The leaf's first key changed,
  so its index node's `(first_time, first_seq)` is rewritten through the
  leaf's `skiplist_node_ref` back-pointer. Ordering is preserved because
  the new first key still sits strictly between the predecessor node's
  leaf and the successor node's.
- **Leaf still populated, slot 0 kept.** Nothing to do beyond compacting
  the value heap.
- **Leaf now empty.** `leaf_unlink` removes it from the doubly-linked
  chain, calls `skiplist_unlink`, and frees the page. Leaving an empty
  leaf in place would be a correctness bug, not just waste: its index
  node's key would no longer describe anything, so a descent could stop
  at it and report a present record as missing.

`skiplist_unlink` needs the *predecessors* of a node whose key it knows
exactly, which is what `descend_ex(..., strict = true)` provides — it
stops at the first node whose key is `>=` the target instead of `>`. Each
level below the node's tower height is repatched to skip it, and
`current_max_level` is recomputed from the head. Node slots themselves
are never recycled, matching the insert path.

Finally the root's `record_count` is decremented and `min_time` /
`max_time` are re-read from `first_leaf` / `last_leaf`.

`spike_db_delete_range` repeatedly finds the lowest key `>= time_lo` and
deletes it while that key is `<= time_hi`. Each pass removes exactly one
record, so progress is guaranteed and no key list has to be materialized
— `O(k log N)` for `k` deletions, which is the right trade for an
operation that runs during corrections rather than on the hot path.

### 4.7 Retention (`spike_db_truncate_before`)

Under the exclusive lock, in one transaction:

1. Walk the leaf chain from `first_leaf`, calling `page_free` on every
   leaf that is empty or whose `max_time < cutoff`.
2. The first surviving leaf is partially truncated by `leaf_drop_below`
   (`memmove` the slot directory down, then compact the value heap) and
   becomes the new `first_leaf` with a cleared `prev_leaf`.
3. Recompute `leaf_count`, `min_time`, `last_leaf` and `record_count` on
   the root page.
4. For each skip-list level, walk the chain and splice out nodes whose
   target leaf no longer belongs to this symbol, is empty, or has
   `max_time < cutoff`. Surviving nodes get their `first_time` refreshed,
   and `current_max_level` is recomputed from the head's `forward[]`.

Because leaves are freed before any node page is allocated in the same
transaction, the freed pages cannot be confused with live ones during the
splice pass.

---

## 5. Transactions and crash safety

There is no write-ahead log. Durability comes from copy-on-write page
allocation plus a double-buffered meta page.

**Commit** (`txn_commit`):
1. `cache_flush_all` writes every dirty page, sorted by page ID so the
   kernel sees ascending, coalescible writes.
2. `fdatasync` / `FlushFileBuffers` — data pages are now durable.
3. Copy the active meta to the inactive slot, increment `txn_id` and
   `reader_epoch`, recompute the CRC, write that meta page.
4. Flush again — the meta is now durable.
5. Flip the in-memory `active_meta` marker. This publishes the
   transaction.

`SPIKEDB_WRITE_NOSYNC` skips steps 2 and 4. Ordering and atomicity are
unchanged — the meta flip still publishes all-or-nothing — but a power
loss can lose recent commits, because the meta page may reach the platter
before the data it refers to. Two fsyncs per commit is right for a batch
loader and wrong for a tick ingester committing every few milliseconds,
and the exchange is the system of record anyway: on restart the ingester
resumes from the cursor in `MetaPage.user_meta`. `spike_db_sync` flushes
on the caller's own schedule.

**Rollback** (`txn_rollback`) and a failed commit both go through
`txn_discard`: drop the dirty cache slots, then re-read *both* meta pages
and re-pick the active one, which restores `freelist_head` and the bump
watermark (the transaction mutates the in-memory active meta in place, so
re-reading is the cheapest way to undo it). Re-reading both rather than
just the active one matters when a commit failed after the new meta had
already reached the disk: the handle then adopts the state the file
actually has instead of a mutated copy of the old one. Pages allocated by
the failed transaction stay unreachable and are effectively leaked until
reused via the watermark.

**Recovery** (`spike_db_open`): read both meta pages, validate magic and
CRC, and adopt the valid one with the higher `txn_id`. If neither
validates, the open fails. Anything written by a half-completed
transaction is unreachable from the surviving meta.

#### Where this is not yet true

Step 1 writes **modified pre-existing pages in place** — only pages the
transaction *allocated* are new. Those pages are reachable from the old
meta at the same page ids, so writing them publishes them, flip or no
flip. A crash or I/O error partway through step 1 therefore leaves some
of the batch visible under the old `txn_id`.

This is measured, not theoretical: `test_io_write_failure_unwinds` fails
the second page write of a commit and finds a file whose root page reports
the new record count while the leaf chain is still the old one —
`spike_db_verify` calls it corrupt. The next successful commit rewrites
both consistently and repairs it, but a reader in between sees a database
that does not verify.

So the accurate statement today is: **the meta flip is atomic; the flush
that precedes it is not.** Making a batch genuinely all-or-nothing needs
the pre-images of in-place pages written to a log area and fsynced before
step 1, with recovery restoring them when it finds an uncommitted log.
That is a format change (a log region plus a meta pointer to it) and is
the top open item.

Torn writes inside the data region are detected — every page carries a
CRC-32C trailer — but not repaired.

---

## 6. Page cache

A fixed pool of 64 KB slots, backed by one 4096-byte-aligned allocation
(`_aligned_malloc` / `posix_memalign`). No `mmap`, so the file may grow far
beyond RAM.

- **Lookup**: open-addressed hash table sized to the next power of two
  ≥ 2× capacity, keyed by page ID → slot index.
- **Pinning**: `page_pin` (reads from disk on miss), `page_pin_zero`
  (skips the read for freshly allocated pages), `page_unpin`,
  `page_dirty`. All page access goes through these.
- **Eviction** (`cache_evict`): a clock sweep with two passes. Pass 1
  takes a clean unpinned victim (no I/O). Pass 2 spills a dirty unpinned
  victim to disk and reuses it, but **only if the slot is `fresh`** —
  i.e. its page ID was allocated by the current transaction. Spilling
  such a page is safe because it is not reachable from any committed meta
  page, so writing it early does not publish it; publication only happens
  at the meta flip. Pre-existing pages are modified in place, so writing
  their mid-transaction bytes would destroy the copy that a rollback (or a
  concurrent reader on the older meta) still needs.
- **Cache exhaustion**: if no victim can be found, `db->cache_oom` is set,
  the transaction is rolled back, and the caller sees `SPIKEDB_FULL`. In
  practice this means leaked pins, such as an iterator that was never
  closed.
- **Rollback** (`cache_discard_dirty`) invalidates both dirty slots and
  clean-but-`fresh` slots, since the latter hold spilled mid-transaction
  bytes whose page IDs are about to return to the freelist.

I/O is `pread` / `pwrite` on POSIX and `ReadFile` / `WriteFile` with
`OVERLAPPED` offsets on Windows. Those five calls — read, write, fsync,
lock, unlock — are reached through a per-handle function table
(`SpikeDB_IoOps` in `src/spike_db_internal.h`) that defaults to the real
implementations. The test suite installs a wrapper to fail the Nth call of
a kind, which is the only way to reach the error-handling half of the
code; nothing in a release build changes but one indirect call per page.

---

## 7. Concurrency

**Within a process**, a `SpikeDB*` handle is *not* thread-safe. Give each
thread its own handle or serialize access externally.

**Across handles and processes**, coordination uses a single OS file range
lock on one byte at file offset `1 << 40` — far past any real data:

- Readers (`get`, `scan`, `min_time` / `max_time` / `count`) take it
  **shared**.
- Writers (`write`, `truncate_before`) take it **exclusive** for the whole
  transaction.
- `spike_db_scan` holds the shared lock from creation until
  `spike_db_iter_close`, so a long-lived iterator blocks the writer for
  its whole life. `SPIKEDB_SCAN_NONBLOCKING` takes and releases it once
  per chunk instead (see §4.5).

An iterator holds no page pins between calls in either mode, so releasing
the lock between chunks is safe: the caller only ever sees bytes already
copied into the iterator's own buffer.

Implemented with `LockFileEx` on Windows and `fcntl(F_OFD_SETLKW)` on
Linux, with an `F_SETLKW` fallback.

After acquiring the lock, every operation calls `db_refresh_meta`, which
re-reads both meta pages and switches to the newer valid one. If another
process committed, clean unpinned cache slots are invalidated so stale
pages are not served.

---

## 8. Limits and constants

These are the values the code actually computes; `SPDB_STATIC_ASSERT`
checks at compile time that each structure and capacity stops short of the
checksum trailer.

| Constant | Value |
|---|---|
| `SPIKEDB_PAGE_SIZE` | 65536 (65532 usable + 4-byte CRC trailer) |
| `SPIKEDB_MAX_LEVEL` | 16 |
| `SPIKEDB_SYMDIR_PAGES` | 16 (4095 slots per page, 65520 total) |
| `SPIKEDB_RESERVED_PAGES` | 18 |
| Max value length | 65000 bytes |
| Max fixed `record_size` | 16384 bytes |
| Transactional user metadata | 4096 bytes |
| Min cache size | 16 pages (1 MiB); smaller requests are raised |
| Skip-list nodes per node page | 442 |
| Page IDs per freelist page | 16379 |
| Leaf header | 64 bytes |
| Records per leaf, variable | 779 at 64 B values, 1487 at 24 B |
| Records per leaf, fixed | 861 at 64 B (+10.5%), 1818 at 24 B (+22.3%) |

Status codes: `SPIKEDB_OK` (0), `SPIKEDB_NOT_FOUND` (-1),
`SPIKEDB_ERROR` (-2), `SPIKEDB_FULL` (-3), `SPIKEDB_CORRUPT` (-4),
`SPIKEDB_INVAL` (-5). Open flags: `SPIKEDB_OPEN_READONLY` (the file must
already exist and is never created).

---

## 9. Rationale notes

- **Skip list of *leaves*, not records.** Indexing individual tick records
  would explode index size and I/O. Indexing leaf pages gives B+tree-like
  scan behavior with skip-list-simple inserts and no rebalancing.
- **Values inside the leaf.** No separate heap means a scan touches one
  page kind, and crash safety has one fewer structure to keep consistent.
  The cost is a 65000-byte value ceiling.
- **No `mmap`.** Target files run into hundreds of GB; an explicit cache
  bounds resident memory and keeps I/O behavior predictable.
- **No WAL.** Batches are large and infrequent enough that two flushes per
  commit are cheaper than writing every record twice.
- **Duplicate `(symbol, time, seq)` rejected rather than overwritten.** Silent
  overwrite hides ingest bugs; rejection fails the whole batch atomically.
- **Per-symbol RNG on disk.** Tower heights stay reproducible across
  process restarts without any global state.

---

## 10. Not implemented

These are known gaps, not planned commitments:

- Asynchronous read-ahead during long scans (`spike_db_prefetch` warms the
  cache synchronously instead).
- SIMD intra-leaf search (the scalar binary search is far below I/O cost).
- Online compaction of leaf chains fragmented by heavy mid-time inserts.
- MVCC / snapshot isolation. `SPIKEDB_SCAN_NONBLOCKING` gives
  read-committed scans that do not stall the writer, but there is still
  no stable snapshot (see §4.5).
- Reuse of individual skip-list node slots. A symbol's node pages are
  freed wholesale by `spike_db_symbol_drop`, but slots inside a live
  symbol's node page are never recycled.
- `O_DIRECT` / `FILE_FLAG_NO_BUFFERING`.
- Big-endian hosts, thread safety on a shared handle, compression.
