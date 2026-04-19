# SpikeDB

[![CI](https://github.com/ZacWalk/spike-db/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/spike-db/actions/workflows/ci.yml)


A single-file, embeddable, append-dominant time-series store for financial
market data. Composite primary key `(symbol_u64, time_u64)`, sorted by
`time` within each `symbol`. Designed for one ingester process and many
concurrent reader processes (bots, training jobs, dashboards).

> Status: pre-1.0. API stable in shape; on-disk format may still change
> until v1.0.

---

## Why SpikeDB?

Financial tick / candle data has a very specific shape that general-purpose
KV stores handle poorly:

- **Two-part composite key.** Every record is `(symbol, time)` and the
  natural query is "give me all of `BTCUSDT` between `t0` and `t1`."
- **Append-dominant, mostly time-ordered.** Live ticks arrive nearly sorted
  but can occasionally land out of order (network reordering, late fills).
- **One writer, many readers.** A single ingester writes; many bots and
  training pipelines stream the same data concurrently from other
  processes — sometimes hours-long range scans.
- **"Anything new since X?"** Every bot polls "what is the latest tick I
  have?" thousands of times per second. This must be effectively free.
- **Massive files.** Per-symbol per-year tick volumes run
  into hundreds of GB. Memory-mapping is not an option — it would either
  thrash the page cache or limit the file to RAM.

SpikeDB is built around these constraints:

| Property                                          | Why it matters |
|---------------------------------------------------|----------------|
| Composite `(symbol, time)` key                    | No string parsing on every lookup; one `uint64_t` compare per level |
| One skip list of leaves **per symbol**            | Symbols are isolated; scans never traverse other symbols' data |
| 64 KB pages, sorted records, doubly-linked        | A single skip-list descent positions the iterator; it then walks `next_leaf` until done — close to memcpy speed |
| Atomic batch writes (CoW + double-buffered meta)  | Crash-safe without a WAL; the ingester restarts cleanly mid-batch |
| `spike_db_max_time(symbol)` is one cached page    | Bots poll latest-timestamp for free |
| OS file range locks                               | Multiple processes coexist; readers block only during the writer's commit |
| No mmap; page cache with `pread`/`ReadFile`       | Files can grow far beyond RAM |
| `spike_db_truncate_before(symbol, t)`             | Easy retention — drop yesterday's ticks atomically |

**Initial use case:** ingest live exchange tick streams into a single file,
train models off the same file across multiple processes, then run live
strategies against a continuously-updated copy in production.

---

## Quick example

```c
#include "spike_db.h"

SpikeDB* db;
spike_db_open(&db, "ticks.spdb", /* cache pages 64 KB */ 1024, 0);

/* Ingest a batch of ticks atomically */
SpikeDB_Batch* b = spike_db_batch_create();
for (int i = 0; i < n_ticks; i++) {
    spike_db_batch_put(b, ticks[i].symbol, ticks[i].time_ns,
                       &ticks[i].payload, sizeof(ticks[i].payload));
}
spike_db_write(db, b);   /* atomic; either all commit or none */
spike_db_batch_destroy(b);

/* "Anything new since I last looked?" — single page read, no I/O */
uint64_t latest;
spike_db_max_time(db, BTCUSDT, &latest);

/* Stream the latest 60 seconds */
SpikeDB_Iter* it = spike_db_scan(db, BTCUSDT,
                                 latest - 60ULL * 1000000000ULL, latest);
uint64_t t;
const void* val;
size_t len;
while (spike_db_iter_next(it, &t, &val, &len)) {
    process_tick(t, val, len);
}
spike_db_iter_close(it);

spike_db_close(db);
```

---

## Architecture

### One skip list of leaves per symbol

```
              ┌───────────── file ─────────────┐
              │  Pages 0..1   Meta A / Meta B  │  double-buffered, CRC32
              │  Pages 2..17  Symbol directory │  open-addressing hash → SymbolRoot
              │  Pages 18..   Skip-list nodes  │
              │               + Leaf pages     │
              │               + Free-list      │
              └────────────────────────────────┘
```

Every symbol owns its own skip list. The skip list indexes **leaf pages**,
not individual records. Leaves additionally form a doubly-linked list in
time order, so a range scan does a single `O(log N)` descent and then walks
`next_leaf` until the upper bound — close to sequential I/O even when the
hot data is gigabytes deep on disk.

### 64 KB leaf page

```
┌─ LeafHeader ─────────────────────────────────────────────────────────┐
│ symbol, min_time, max_time, record_count,                            │
│ value_heap_bottom, prev_leaf, next_leaf, skiplist_node_ref           │
├─ LeafSlot[record_count] (sorted by time, grows up) ──────────────────┤
│ { time, value_offset, value_len }   (16 bytes per slot)              │
│                                                                      │
│                       ─── free space ───                             │
│                                                                      │
├─ value heap (grows down) ────────────────────────────────────────────┤
│ packed value bytes                                                   │
└──────────────────────────────────────────────────────────────────────┘
```

A 64 KB leaf holds ~3000–4000 typical tick records. Iteration is just a
binary search inside the slot directory followed by sequential reads.

### `SymbolRoot` page (one per symbol)

Holds `min_time` / `max_time` / `record_count` / `leaf_count`, the skip
list head's `forward[]` array, the per-symbol RNG state used to pick
random tower heights, and a "current node page" pointer for packing many
skip-list nodes into a single 64 KB page. `spike_db_max_time` /
`min_time` / `count` are a single read of this page — usually one
warm-cache hit.

### Insert paths

- **Append fast path.** When `time > leaf.max_time`, the last leaf is
  full, and there is no next leaf, the writer allocates a fresh leaf and
  drops the record there with no data movement. This is the dominant
  case for ingest.
- **Mid-leaf insert.** When the time falls inside the leaf, the slot
  directory does a `memmove` on a few bytes and the value is appended to
  the value heap. Existing values never move.
- **Split.** A full leaf splits at the median time, copies half the
  records to a new leaf, fixes the linked-list pointers, and adds a new
  skip-list index node at a random level.

### Crash safety (no WAL)

1. All page mutations during a transaction land on shadow copies in the
   page cache (CoW). New page IDs come from a transaction-local
   allocation list so a failed batch returns them to the freelist.
2. Commit:
   - Flush every dirty page → `FlushFileBuffers` / `fdatasync`.
   - Write the *inactive* meta page with `txn_id + 1`.
   - `FlushFileBuffers` / `fdatasync` again.
   - Atomically flip the in-memory active-meta marker.
3. Recovery picks the meta page with the highest valid CRC32-checked
   `txn_id`; everything written by a half-done transaction is unreachable
   from that meta and is reclaimed via the freelist.

### Multi-process concurrency

A single byte at file offset `1 << 40` (well past any real data) is the
lock arena. Writers (`spike_db_write`, `spike_db_truncate_before`) take
**exclusive**; readers (`spike_db_get`, `spike_db_scan`,
`spike_db_max_time` / `min_time` / `count`) take **shared**. Iterators
hold the shared lock for their lifetime so the writer cannot free leaves
out from under them. After acquiring a lock, every operation re-reads
both meta pages and switches to whichever is newer (`db_refresh_meta`),
invalidating clean unpinned cache slots if another process committed.

Implemented with `LockFileEx` on Windows and `fcntl(F_OFD_SETLKW)` on
Linux (with `F_SETLKW` fallback).

### Page cache

Clock-sweep eviction over a fixed-size pool of 64 KB slots. O(1)
hash-table lookup keyed by page ID, pin/unpin reference counting, dirty
flag, copy-on-write. Backed by `pread` / `pwrite` on Linux and
`ReadFile` / `WriteFile` with `OVERLAPPED` offsets on Windows. **No
`mmap`** — files can grow far beyond RAM without paging behavior.

---

## File layout

| Pages   | Contents                                                          |
|---------|-------------------------------------------------------------------|
| 0–1     | Double-buffered meta pages (`MetaPage`, CRC32-protected)          |
| 2–17    | Symbol directory (open-addressing hash, ~65k slots)               |
| 18+     | `SymbolRoot` pages, skip-list node pages, leaf pages, freelist    |

All pages are 64 KB.

---

## API

```c
/* open / close */
SpikeDB_Status spike_db_open (SpikeDB** out, const char* path,
                              uint32_t cache_pages_64k, uint32_t flags);
void           spike_db_close(SpikeDB* db);

/* point lookup */
SpikeDB_Status spike_db_get  (SpikeDB* db, uint64_t symbol, uint64_t time,
                              void** value_out, size_t* len_out);
void           spike_db_free (void* ptr);

/* atomic batch */
SpikeDB_Batch* spike_db_batch_create (void);
void           spike_db_batch_destroy(SpikeDB_Batch* b);
void           spike_db_batch_clear  (SpikeDB_Batch* b);
size_t         spike_db_batch_count  (const SpikeDB_Batch* b);
SpikeDB_Status spike_db_batch_put    (SpikeDB_Batch* b,
                                      uint64_t symbol, uint64_t time,
                                      const void* value, size_t len);
SpikeDB_Status spike_db_write        (SpikeDB* db, SpikeDB_Batch* b);

/* range scan */
SpikeDB_Iter*  spike_db_scan      (SpikeDB* db, uint64_t symbol,
                                   uint64_t time_lo, uint64_t time_hi);
bool           spike_db_iter_next (SpikeDB_Iter* it, uint64_t* time_out,
                                   const void** value_out, size_t* len_out);
void           spike_db_iter_close(SpikeDB_Iter* it);

/* hot polling helpers (single cached page read each) */
SpikeDB_Status spike_db_max_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_min_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_count   (SpikeDB* db, uint64_t symbol, uint64_t* out);

/* retention */
SpikeDB_Status spike_db_truncate_before(SpikeDB* db, uint64_t symbol,
                                        uint64_t time_exclusive);

/* diagnostics */
void spike_db_stats(SpikeDB* db, SpikeDB_Stats* out);
```

**Return codes:** `SPIKEDB_OK` (0), `SPIKEDB_NOT_FOUND` (-1),
`SPIKEDB_ERROR` (-2), `SPIKEDB_FULL` (-3), `SPIKEDB_CORRUPT` (-4),
`SPIKEDB_INVAL` (-5).

**Open flags:** `SPIKEDB_OPEN_READONLY`.

**Semantics worth noting:**

- `(symbol, time)` is unique. Inserting the same `(symbol, time)` twice
  is rejected with `SPIKEDB_INVAL`; the entire offending batch is rolled
  back atomically.
- `spike_db_batch_put` accepts entries in any order. The engine sorts
  them by `(symbol, time)` before applying.
- The pointer returned in `*value_out` from `spike_db_iter_next` is owned
  by the iterator and is valid only until the next `iter_next` /
  `iter_close` call. `spike_db_get` returns a `malloc`'d buffer that the
  caller must release with `spike_db_free`.
- Maximum value length is 65000 bytes (a value must fit in a leaf with
  room for at least its own slot directory entry).

---

## Building

Single-file C11 implementation; the only dependencies are libc and the
OS file APIs.

```powershell
# Windows (from any PowerShell — script auto-imports MSVC env)
./run_tests.ps1                  # AVX2 build, full test suite
./run_tests.ps1 -Arch AVX512
./run_tests.ps1 -Quick           # skip the long benchmark
./build.ps1                      # build only (must be in a Developer PS)
```

```bash
# Linux / WSL
gcc -O2 -mavx2 -msse4.2 -I src \
    src/spike_db.c src/test_spike_db.c \
    -o build/test_spike_db
./build/test_spike_db
```

The build script auto-detects Visual Studio installations including the
2026 (year directory `18`) layout.

---

## Performance

100k records of 64 B each (single-process, warm cache, 32 MB cache pool):

| Platform                | Ingest        | Scan (warm)   | Pages used |
|-------------------------|---------------|---------------|------------|
| Windows MSVC AVX2       | ~1.5 M rec/s  | ~52 M rec/s   | 143        |
| WSL GCC AVX2 (`/mnt/c`) | ~530 K rec/s  | ~38 M rec/s   | 143        |

`spike_db_max_time` is a single cached page read once the `SymbolRoot`
page is warm.

---

## Status / roadmap

Implemented:

- 64 KB page format, page cache (`pread`/`ReadFile`, no mmap)
- Symbol directory, per-symbol skip list of leaves, doubly-linked leaf chain
- Atomic batch writes (CoW + double-buffered CRC32 meta, no WAL)
- Range-scan iterator with O(log N) descent then sequential walk
- `max_time` / `min_time` / `count` — single cached page read
- `truncate_before` for retention (atomic; reclaims whole leaves and
  partially truncates the boundary leaf)
- OS file range locking for multi-process safety (Windows `LockFileEx`,
  Linux `fcntl(F_OFD_SETLKW)`)
- Skip-list node packing (many nodes per 64 KB page)

Not yet:

- Per-leaf CRC32 (torn-write detection at sub-meta granularity)
- Asynchronous read-ahead during long scans
- SIMD intra-leaf binary search (current scalar binary search is already
  ≤ ~12 comparisons on a full leaf)
- Online compaction of fragmented leaf chains after heavy mid-time
  inserts
- Snapshot isolation across long-running readers (currently they take a
  shared range lock for the iterator's lifetime)
- `O_DIRECT` / `FILE_FLAG_NO_BUFFERING` knob

