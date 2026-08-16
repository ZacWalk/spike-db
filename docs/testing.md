# SpikeDB Testing Plan

SpikeDB is a storage engine: a bug does not throw an exception, it silently
returns the wrong tick six months later, or eats a day of ingest on a power
cut. The test strategy is therefore built around three claims we must be
able to defend:

1. **Nothing is ever silently wrong.** Every read either returns the exact
   bytes that were committed or an error code.
2. **Every commit is atomic.** After any crash, at any instruction, the
   file opens and contains either all of a batch or none of it.
   **This one is currently false** — §3 found that a commit interrupted
   mid-flush publishes part of the batch, because modified pre-existing
   pages are written in place before the meta flip. Closing it is the top
   open item; see §6 and docs/design.md §5.
3. **Every failure path unwinds cleanly.** An I/O error, a full cache, or a
   failed allocation leaves the file and the handle usable, with no leaked
   pins, pages, or memory.

See [docs/design.md](design.md) for the mechanisms these claims rest on.

---

## 0. Where we are today

`src/test_spike_db.c` holds 63 deterministic tests on a small built-in
harness (`TEST` / `CHECK` / `PASS`), run by `dd.ps1` on MSVC and GCC. They cover open/close, put/get, persistence across reopen, ordered
and bounded scans, multi-symbol isolation, out-of-order and mid-leaf
inserts, `min`/`max`/`count`, batch atomicity on duplicates, retention,
read-only opens, oversized values, 300-symbol directory stress, torn meta
recovery, five cache dirty-spill scenarios, the `seq` tiebreaker, as-of
lookups, corrections and deletes, the transactional ingest cursor,
non-blocking and reverse scans, merged multi-symbol replay, page-checksum
corruption detection, `spike_db_verify`, fixed-width leaves and
`read_range`, symbol drop, backup, and durability control.

Five of them defend the caller rather than the engine, because the first
thing a new user does is hold the API wrong:

| Test | Claim |
|---|---|
| `test_crc32_paths_agree` | The SSE4.2 and table-driven CRC-32C paths agree at every length and alignment, and both match the standard check value — a file written on one machine stays readable on another |
| `test_api_rejects_null_arguments` | Every public entry point rejects a NULL handle, batch, iterator or output pointer instead of dereferencing it; destructors accept NULL |
| `test_api_misuse_is_diagnosed` | Replaying an uncleared batch, exceeding the value limit, re-declaring a fixed width, writing a wrong-width value, reversing a range, seeking a merged iterator and overflowing the ingest cursor all fail cleanly and atomically |
| `test_readonly_rejects_mutations` | Every mutating call on a read-only handle returns an error, reads and backup still work, and the txn id never moves |
| `test_boundary_keys_and_values` | `symbol == 0`, `time == 0`, `time == UINT64_MAX`, `seq == UINT32_MAX`, zero-length values and degenerate/empty ranges all round-trip through the file |

And five reach the failure paths through the injectable I/O table:

| Test | Claim |
|---|---|
| `test_io_write_failure_unwinds` | Failing the Nth page write of a commit, for every N, reports the failure, leaves the handle answerable and the file openable; a fault before any write lands is fully atomic |
| `test_io_fsync_failure_is_reported` | A failed data barrier is reported; a failed meta barrier leaves the old or the new state, never a third one, and the file still takes writes |
| `test_io_read_failure_is_reported` | A failed read produces an error, never a wrong answer or a partial scan reported as complete; clearing the fault restores normal service |
| `test_lock_failure_is_reported` | Nothing proceeds without the lock it asked for, and no lock is leaked when acquisition fails |
| `test_handle_invariants_hold` | `spike_db_internal_check` is clean after open, write, scan, truncate, delete, drop, and after an aborted transaction |

Verification passes CI runs on every push:

| Pass | Command | Catches |
|---|---|---|
| Default | `dd.ps1 run` | Functional regressions on MSVC and GCC |
| Audit | `dd.ps1 audit` | Cache/hash-table/pin invariants at the exit of every mutating call (§5) |
| ASan + UBSan | `dd.ps1 asan` | Out-of-bounds, misaligned loads, leaks |
| Low-memory | `dd.ps1 lowmem` (16 and 32 pages) | Leaked page pins, which surface as `SPIKEDB_FULL` once the clock sweep runs out of victims |
| Layout guards | any build | `SPDB_STATIC_ASSERT` fails the build if a packed struct changes size or a capacity would overrun the checksum trailer |

The low-memory pass is the cheap proxy for the pin-balance audit in §5:
at 16 pages (the 1 MiB minimum) any unmatched `page_pin` starves the sweep
within a few operations.

That is a good functional floor and a moderate reliability floor.
Everything tested today is a *success* path plus a handful of hand-built
failure cases. The plan below adds the machinery to test failure
systematically.

---

## 1. Build configurations

Every configuration must run in CI. Bugs live in the gaps between them.

| Build | Purpose | Notes |
|---|---|---|
| **Debug** | Invariant checking | `assert()` active, structural audits enabled (§5), no optimization |
| **Release** | What we actually ship | `/O2` or `-O2`; validates the optimized machine code, not the source |
| **Coverage** | Branch-coverage measurement (§9) | GCC `--coverage`, no optimization |
| **ASan + UBSan** | Memory and UB errors | GCC/Clang; also catches leaked allocations at exit |
| **Valgrind (Linux)** | Uninitialized on-disk bytes | Catches padding/reserved fields written un-zeroed into the file |

Cross-product with both compilers (MSVC, GCC) and both ISA settings
(`AVX2`, `AVX512`). The ISA axis matters because CRC-32C has a hardware
path and a software fallback that must produce identical checksums; a
divergence there makes files written on one machine unreadable on another.
`test_crc32_paths_agree` covers that directly by comparing the two paths in
one process through `spike_db_internal_crc32c`.

---

## 2. Layer 1 — Deterministic functional tests

Keep extending the existing harness. Concrete gaps, in priority order:

**Boundary values**
- Value at exactly the leaf-capacity boundary, and the sequence of value
  sizes that leaves exactly 0, 1, and 16 bytes of free space in a leaf.
- A single batch that fills a leaf exactly, then one more record.
- Scans whose range falls entirely before, after, or between two leaves
  (`test_boundary_keys_and_values` covers between two *records*).

**Structural transitions**
- Splitting the *first* leaf, the *last* leaf, and a middle leaf.
- Insert that lands before `min_time` of the whole symbol.
- Filling one node page to `NODE_PAGE_CAPACITY` (455 nodes) so a second
  node page is allocated, then descending across the boundary.
- Driving `current_max_level` to `SPIKEDB_MAX_LEVEL` and inserting above it.
- Symbol directory: fill enough slots to force long linear probes and
  page-boundary wraparound; confirm the full-directory case returns
  `SPIKEDB_FULL` rather than looping.

**Retention edge cases**
- `truncate_before` with a cutoff below `min_time` (no-op), equal to a leaf
  boundary, inside the first leaf, at `UINT64_MAX`, and on an unknown symbol.
- Truncate → reinsert into the freed page range → verify the skip list did
  not retain a stale node pointing at a recycled page. This is the sharpest
  edge in the whole engine.

**Freelist**
- Free enough pages to fill a freelist page (16380 IDs) and spill to a
  second; then allocate them all back and confirm the chain unwinds.
- Verify the file does not grow when free pages are available.

**API contract**
- `spike_db_close` with an iterator still open, and with a transaction
  in flight. Today this is undefined; decide whether it should be, and
  either define and test it or say so in the header.
- A handle used from two threads, to confirm the single-threaded contract
  is at least detected under a debug build rather than silently corrupting.

---

## 3. Layer 2 — An injectable I/O boundary

**Done.** `io_read`, `io_write`, `io_fsync`, `file_lock` and `file_unlock`
dispatch through a per-handle `SpikeDB_IoOps` table (declared in
`src/spike_db_internal.h`, defaulting to `spike_db_internal_io_os`). A test
installs a wrapper with `spike_db_internal_set_io` and fails the Nth call
of one kind. No public API change; one indirect call per page I/O.

What it already proved, on its first run:

- **Commits are not atomic against a mid-flush failure.** `cache_flush_all`
  writes modified pre-existing pages in place, and those pages are
  reachable from the *old* meta, so an abort after the second write leaves
  the batch half-published — measured as a root page reporting 6000 records
  over a 3-leaf chain, which `spike_db_verify` rejects. See §6.
- **A failed commit left the handle's in-memory meta mutated.**
  `txn_commit_ex`'s failure path discarded the dirty pages but not the
  bump-pointer/freelist mutations, and `db_refresh_meta` short-circuits
  when the on-disk `txn_id` has not moved — so the *next* commit built on
  a watermark the file knew nothing about. Fixed by routing both rollback
  and failed commit through `txn_discard`, which re-reads both meta pages.

Still to do on this layer:

- Sweep every call site, not just the four scenarios covered: short reads,
  garbage reads, and failures inside `spike_db_open`, `backup` and
  `truncate_before`.
- Writes that succeed but are silently reordered — the fsync barriers are
  the only ordering that may be relied upon.
- Every iteration should end with `spike_db_internal_check` *and* a full
  `spike_db_verify`, which today only the final state gets.

The mechanical form is a loop: run the same workload with the fault set to
trigger on call 1, 2, 3, … until the workload completes without hitting the
fault. **Each iteration must start from a pristine file** — an aborted
commit can damage the database (see above), and a damaged database answers
the next iteration's question about the wrong thing.

---

## 4. Layer 3 — Allocation failure

SpikeDB calls `malloc`, `calloc`, `realloc`, `posix_memalign` and
`_aligned_malloc` in a dozen places: handle setup, cache storage, the
hash table, `txn_allocated` growth, `cache_flush_all`'s key array, the
`leaf_compact` scratch page, iterator buffers, batch growth, and the buffer
returned by `spike_db_get`.

Route these through a test-controllable allocator and run the same
"fail the Nth allocation, for every N" sweep. Each iteration asserts:

- The API returns an error rather than dereferencing NULL.
- No memory is leaked (ASan/LeakSanitizer verifies this for free).
- The handle is still usable, or was cleanly torn down.
- The on-disk file is unchanged if the failure occurred pre-commit.

---

## 5. Layer 4 — Structural invariant auditing

**Partly done.** `spike_db_internal_check` walks everything about the
handle that never reaches disk, and `SPDB_AUDIT` calls it from the exit of
`spike_db_open`, `spike_db_write`, `spike_db_symbol_define`,
`spike_db_truncate_before`, `spike_db_delete_range` and
`spike_db_symbol_drop` when the build defines `SPIKEDB_AUDIT`
(`dd.ps1 audit`). CI runs the suite both with and without, since the audit
perturbs nothing but is worth keeping off the default path.

Covered today (page cache):
- Every valid slot is in the hash table exactly once and maps back to
  itself; every hash entry points at a valid slot; occupancy matches.
- Total pin count is zero when no operation is in flight — this catches
  leaked pins at the moment they leak instead of as a `SPIKEDB_FULL` from
  an unrelated write days later.
- No slot is dirty or marked `fresh` outside a transaction, and no
  transaction is left in flight.

Still to do — the file-level walk. `spike_db_verify` already covers most of
it (leaf chains, slot ordering, value extents, cached counters, skip-list
levels, free list), so the remaining work is to call it from the audit hook
rather than to reimplement it, plus the parts it does not check:

- Every page is reachable exactly once as a symbol root, node page, leaf,
  or freelist entry — no page is both live and free, none is orphaned.
- Freelist contains no duplicates and no reserved page (< 18).
- No pre-existing (non-`fresh`) page has been written to disk before the
  meta flip. This is the invariant §3 proved is violated by design; the
  audit should assert it once the undo log exists.

---

## 6. Layer 5 — Fix and then test crash atomicity

This is now the top item, because §3 turned it from a suspicion into a
measurement: **a commit interrupted mid-flush publishes part of the batch.**
`cache_flush_all` writes modified pre-existing pages in place (design.md
§5), and those pages are reachable from the old meta at the same page ids,
so the meta flip is not what makes them visible — the write is.

**Fix first.** Testing harder cannot make this pass. The shape of the fix:
before step 1 writes any page that the transaction did not itself allocate,
append that page's pre-image to a log region, fsync the log, and record its
extent in the meta being written. Recovery replays the log backwards when
it finds one belonging to an uncommitted transaction. That is a format
change — a log region plus a meta pointer — so it needs a `SPIKEDB_MAGIC`
bump and a note in the README.

**Then test it** with a recording layer on the I/O table from §3:

1. Run a workload, recording every write (page ID, bytes) and every fsync
   as an ordered log.
2. For each prefix of that log, materialize a candidate file: apply all
   writes before the last fsync barrier, then apply an arbitrary subset of
   the writes after it, in an arbitrary order, optionally tearing one page
   into 4 KB sectors with a mix of old and new content.
3. Open the resulting file and assert:
   - Open succeeds (or returns `SPIKEDB_CORRUPT` only if *both* meta pages
     were mangled).
   - The visible contents equal either the pre-batch or the post-batch
     state — never a mixture, never a partially applied batch.
   - `spike_db_internal_check` passes.
   - A subsequent write commits successfully on top of the recovered file.

Enumerate exhaustively for small workloads; sample pseudo-randomly with a
recorded seed for large ones. Cover at minimum: a plain append batch, a
batch that splits leaves, a batch that allocates a node page, a batch that
triggers a dirty spill, and a `truncate_before`.

**Torn writes deserve their own verdict.** A 64 KB page is not written
atomically by any real device. Every page carries a CRC-32C trailer, so a
torn page is *detected* on read — but detection turns a lost write into a
`SPIKEDB_CORRUPT`, not into a recovered database. The log above is what
turns it into recovery.

---

## 7. Layer 6 — Model-based randomized testing

The functional tests check the cases we thought of. This one checks the
cases we did not.

Maintain a reference model in memory — a sorted array per symbol — and a
driver that applies a pseudo-random operation stream to both the model and
the database:

- weighted operations: batch put (sorted, reversed, shuffled, duplicated),
  get, scan with random bounds, min/max/count, truncate, close/reopen,
  and random cache sizes;
- after each step, compare every query the API can answer against the model,
  and run `spike_db_internal_check`.

Requirements: a seed printed on failure, a `-Seed` switch to replay it, and
a shrinker that reduces a failing stream to a minimal reproducer which is
then promoted into `src/test_spike_db.c` as a permanent named test. Every
bug this layer finds should end its life as a deterministic regression test.

Deliberately skew the distributions toward the ingest profile — mostly
appends with a few percent out-of-order arrivals — and also toward the
pathological one: descending time order, all records at the same time
value, one symbol with millions of records and 60 000 symbols with one each.

---

## 8. Layer 7 — Fuzzing

Two harnesses, both suitable for libFuzzer/AFL and both required to run
clean before any release:

**Format fuzzer.** Take a valid database file, mutate it (flip bits,
truncate, swap pages, corrupt CRCs, plant absurd page IDs, cycles in the
leaf chain, `record_count` larger than the page), then open it and run
every read API. The contract: **never crash, never read out of bounds,
never loop forever.** Returning `SPIKEDB_CORRUPT` is always acceptable;
returning wrong data is not. This layer will require adding bounds checks
on values loaded from disk — page IDs against the allocation watermark,
`record_count` and `value_offset`/`value_len` against page bounds,
`level` against `SPIKEDB_MAX_LEVEL`, node refs against page capacity —
because right now a hostile file is trusted.

**API fuzzer.** Interpret the fuzzer's input bytes as an operation script
(the §7 driver, with the stream supplied instead of generated) and assert
the model equivalence and invariants. Run under ASan/UBSan.

Seed both corpora from the deterministic suite's databases and keep the
corpus in the repo.

---

## 9. Coverage as a gate

Measure branch coverage on the Debug/coverage build and treat uncovered
branches as untested code, not as noise.

- Target 100% branch coverage of `src/spike_db.c`, both directions of every
  conditional. The fault-injection layers exist largely to make the error
  branches reachable; anything still unreachable is either dead code to
  delete or a missing test.
- Add two zero-cost annotation macros so coverage means what we think:
  - `SPDB_TESTCASE(expr)` — a no-op in release, a distinct branch under
    coverage. Place at boundary conditions the compiler would otherwise
    fold: search hitting slot `0` versus `record_count - 1`, free space
    exactly equal to the required size, level exactly `MAX_LEVEL`,
    freelist page exactly full.
  - `SPDB_NEVER(expr)` / `SPDB_ALWAYS(expr)` — for genuinely defensive
    checks. In debug they assert; in release they evaluate to the constant,
    so a defensive branch does not become a permanently uncovered one.
- Record the coverage number in CI and fail the build on a decrease.

---

## 10. Multi-process and concurrency

`test_multiprocess_basic` uses two handles in one process, which does not
exercise the OS lock at all. Add real ones:

- Spawn actual child processes (the test binary re-invoked with a mode
  argument): one writer, several readers, running for a fixed duration.
  Readers assert monotonic `max_time` and that every scan result is
  internally consistent and matches the values the writer claims to have
  written.
- A reader holding an iterator across a writer's commit attempt: assert the
  writer blocks rather than freeing leaves under the reader, and that the
  reader's results stay coherent.
- Kill the writer process mid-batch (`TerminateProcess` / `SIGKILL`) and
  have a fresh process open the file: same assertions as §6, but against a
  real OS crash rather than a simulated one. Run this in a loop with random
  kill delays.
- A reader opened before a commit must see the new data after its next call
  (`db_refresh_meta` correctness), and must never see it before.
- Read-only handle plus writer, and a stale handle whose cached meta is
  several transactions behind.

---

## 11. Algorithmic bounds

Unbounded inputs meet library implementations with unspecified behavior.

- `spike_db_write` sorts the batch with `qsort`, whose implementation
  varies by platform and may recurse deeply or allocate on pathological
  input. Replace it with an in-repo sort that is bounded in stack depth
  (recurse into the smaller partition, loop on the larger, with a heapsort
  fallback) — then test it directly with adversarial permutations of
  millions of entries.
- Bound every loop that walks on-disk structure (leaf chain, skip-list
  level, freelist chain, symbol-directory probe) by a count derived from
  the allocation watermark, so a corrupt file cannot produce an infinite
  loop. §8 will find these if they are not added.
- Test with a deliberately tiny cache (the 16-page minimum) against
  workloads that need far more, to force the spill and eviction paths
  continuously rather than incidentally.

---

## 12. Performance regression guard

`test_large_dataset` already prints ingest and scan rates plus page counts.
Turn the page/hit/miss counts into assertions — they are deterministic —
so that a change which doubles page consumption or destroys the fast-append
path fails the build. Keep the throughput numbers as reported values only;
wall-clock on shared CI runners is not a reliable gate.

---

## 13. Order of work

Each step is useful on its own and unlocks the next:

1. ~~CRC hardware/software equivalence test~~, ~~injectable I/O~~,
   ~~handle invariant audit~~ — done.
2. **Undo log for in-place page writes, and the power-loss simulation that
   proves it** (§6). Everything else is polish next to this.
3. Finish the fault sweeps: every call site, short and garbage reads,
   reordered writes, and a full `spike_db_verify` after every iteration.
4. Allocation-failure sweeps (§4).
5. Bounds-checking of every value loaded from disk.
6. The remaining Layer 1 boundary gaps (leaf-capacity edges, structural
   transitions, freelist spill).
7. Model-based randomized driver, with seed replay and shrinking.
8. Coverage build, annotation macros, and the coverage gate.
9. Fuzz harnesses and a checked-in corpus.
10. Real multi-process and kill tests.
11. Bounded batch sort and loop bounds.

## 14. Rules that keep this from rotting

- Every bug fixed gets a named deterministic test that fails before the fix.
- Randomized and fuzz failures are minimized and promoted into the
  deterministic suite; the random layer never remains the sole proof.
- No test writes outside `tmp/`, and every test cleans up after itself.
- The suite stays dependency-free and self-contained: no test framework, no
  scripting runtime, one C file.
- A change to an on-disk structure requires a test that opens a file written
  by the previous format and fails cleanly.
