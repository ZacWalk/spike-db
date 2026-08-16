# AGENTS.md — SpikeDB

Single-file C11 key-value store for financial time series, keyed by
`(symbol_u64, time_u64, seq_u32)`. Implementation is entirely in
[src/spike_db.c](src/spike_db.c) + [src/spike_db.h](src/spike_db.h);
[src/spike_db_internal.h](src/spike_db_internal.h) exists only so the test
suite can reach inside.

## Read this first

| Question | Document |
|---|---|
| What is it, what's the public API, how do I build it? | [README.md](README.md) |
| How does it work internally (page formats, algorithms, transactions, cache, locking)? | [docs/design.md](docs/design.md) |
| How is it tested, and what is planned? | [docs/testing.md](docs/testing.md) |
| What is the contract for a given function? | Doc comments in [src/spike_db.h](src/spike_db.h) |

Do not restate design details in this file — update
[docs/design.md](docs/design.md) instead.

## Repository layout

```
src/spike_db.c            implementation (all of it)
src/spike_db.h            public header + API contracts
src/spike_db_internal.h   test-only hooks (I/O table, CRC probe, audit)
src/test_spike_db.c       test suite (self-contained harness, no framework)
CMakeLists.txt            build definition (CMake + Ninja)
dd.ps1                    the only script: build, run, audit, asan, lowmem
docs/design.md            internals reference
docs/testing.md           testing strategy and planned work
tmp/                      scratch: test DB files, logs, any temp artifact
build/                    build output, one directory per variant
```

## Build and test

```powershell
./dd.ps1 run                    # build Release + run the suite (default)
./dd.ps1 build                  # build only
./dd.ps1 run -Arch AVX512 -Config Debug
./dd.ps1 audit                  # -DSPIKEDB_AUDIT: structural audit hook
./dd.ps1 asan                   # ASan + UBSan
./dd.ps1 lowmem                 # 16- and 32-page cache passes
./dd.ps1 all                    # everything
./dd.ps1 clean
```

- CMake + Ninja. On Windows `dd.ps1` locates Visual Studio and imports the
  MSVC environment itself (VS year dirs `18`/`2026`/`2025`/`2022`/`2019`),
  because Ninja drives `cl.exe` directly and needs it; it also falls back
  to the `cmake`/`ninja` inside the VS install when they are not on PATH.
- The same script runs under `pwsh` on Linux and in WSL. Build directories
  are namespaced by platform (`build/win-*`, `build/lin-*`) so a Windows
  and a WSL build of the same tree do not fight over one CMake cache.
- Binary: `build/<platform>-<config>-<arch>/test_spike_db[.exe]`.
- `-Quick` only sets `SPIKEDB_TEST_MODE=quick`, which the test binary
  currently ignores. CI uses it, so it must stay harmless.
- CI (`.github/workflows/ci.yml`) runs `dd.ps1 run`, `audit` and `lowmem`
  on `windows-latest` and `ubuntu-latest`, plus `asan` on Linux. Keep both
  paths compiling.

**Always write scratch files to `tmp/`, never the repo root.** The test
suite creates `tmp/` itself and uses `tmp/test_spike_db.dat`.

## Coding conventions

- Public symbols: `spike_db_` / `SpikeDB_`. Internal statics:
  `snake_case`, no prefix. Macros/constants: `SPIKEDB_` or `SPDB_`,
  `UPPER_SNAKE_CASE`.
- On-disk structs live inside `#pragma pack(push, 1)`; little-endian only.
- Platform code is guarded with `#ifdef _WIN32` / `#ifdef _MSC_VER`.
  Every change must build with both MSVC `/W4` and GCC.
- No external dependencies. Only libc plus OS file APIs.
- Return `SpikeDB_Status`: `SPIKEDB_OK` (0), `SPIKEDB_NOT_FOUND` (-1),
  `SPIKEDB_ERROR` (-2), `SPIKEDB_FULL` (-3), `SPIKEDB_CORRUPT` (-4),
  `SPIKEDB_INVAL` (-5).

## Invariants that break things silently

- **All page access goes through `page_pin` / `page_pin_zero` /
  `page_unpin` / `page_dirty`.** Never touch `db->storage` directly.
- **All OS I/O goes through `io_read` / `io_write` / `io_fsync` /
  `file_lock` / `file_unlock`**, which dispatch via `db->io`. Call the
  `*_os` implementations directly only from the default table itself,
  or fault injection stops working.
- **All data page writes go through `page_write_checked`.** It stamps the
  CRC trailer. A page written with a bare `io_write` fails its own
  checksum the next time it is read, which looks like disk corruption.
- **The last 4 bytes of every page are the checksum.** Sizes and offsets
  are relative to `SPDB_PAGE_BODY`, never `SPIKEDB_PAGE_SIZE`. Any layout
  that runs to the end of the page silently eats the CRC.
- **Leaves have two layouts.** `LeafHeader.record_size == 0` is the
  variable-width slot directory; non-zero is a fixed columnar leaf with no
  slots at all. Read through `leaf_find` / `leaf_key_at` / `leaf_val_at`,
  and only call `leaf_slots()` after checking `record_size == 0` —
  otherwise you are reinterpreting the `times[]` column as slots.
- **Every pin needs a matching unpin on every path, including errors.**
  A leaked pin starves the clock sweep and surfaces much later as
  `SPIKEDB_FULL` from an unrelated write. `./dd.ps1 lowmem` and
  `./dd.ps1 audit` both catch these early.
- **A pinned pointer is only valid while pinned.** Any intervening pin can
  evict/reuse the slot, so re-pin rather than caching `uint8_t*` across
  calls. Capture the page id *before* advancing a cursor, or the unpin
  will target the wrong page.
- **Mutations must set `page_dirty`** or they are lost at eviction.
- **Writes happen inside a transaction** (`txn_begin` → mutate →
  `txn_commit` / `txn_rollback`) while holding the exclusive file lock;
  reads take the shared lock and call `db_refresh_meta` first.
- **A failed commit unwinds through `txn_discard`, not just
  `cache_discard_dirty`.** The writer mutates the active meta in place, so
  abandoning a transaction must re-read both meta pages from disk;
  `db_refresh_meta` will not do it for you (it short-circuits when the
  on-disk `txn_id` has not moved).
- **A commit is only atomic at the meta flip, not during the flush.**
  Modified pre-existing pages are written in place beforehand, so an
  interrupted commit can publish part of a batch. Do not add tests or docs
  that assert all-or-nothing under a crash until the undo log exists —
  docs/testing.md §6.
- **Dirty spill safety:** only cache slots marked `fresh` (page allocated
  by the current transaction) may be written to disk mid-transaction.
  Spilling a pre-existing page would destroy the copy that rollback and
  readers on the older meta still need. See §6 of the design doc before
  touching `cache_evict` / `cache_flush_all` / `cache_discard_dirty`.
- **Changing any on-disk struct changes the format.** There is no
  migration path; bump `SPIKEDB_MAGIC`, update the matching
  `SPDB_STATIC_ASSERT` (the build fails until you do), and note it in
  [docs/design.md](docs/design.md) and the README status note.
- **Deleting a symbol directory slot leaves a tombstone.** Zeroing it
  would cut the probe chain of any symbol that collided with it.
- **An iterator borrows its handle and holds a file lock.** It must be
  closed before `spike_db_close`; the library does not track open
  iterators and will not save a caller who gets the order wrong.
- A `SpikeDB*` handle is single-threaded by contract. Do not add locking
  inside the handle to "fix" a threading bug in a caller.

## Tests

`src/test_spike_db.c` uses a tiny built-in harness — do not add a test
framework.

- Declare with `TEST(test_my_thing) { ... }`, assert with
  `CHECK(cond, fmt, ...)` (fails and returns), finish with `PASS()`.
- Call `cleanup()` before/after so `tmp/test_spike_db.dat` does not leak
  between tests. A `CHECK` failure returns immediately, so a failing test
  leaks its handle and every later test then runs against stale data —
  `cleanup()` warns when it cannot delete the file. Fix the *first*
  failure before believing any that follow it.
- Register the test by adding `run_test_my_thing();` to `main`, in order.
- Default cache for tests is `TEST_CACHE = 512` pages (32 MiB); override
  with `./dd.ps1 lowmem` for the low-memory pass.
- Fault injection: install `g_fault_ops` with `spike_db_internal_set_io`
  and `fault_arm(FAULT_WRITE, n)`. Start every fault scenario from
  `fresh_db_with(...)` — an aborted commit can leave the file damaged, and
  a damaged file answers the next question about the wrong thing.
- Any behavior change needs a test; crash-safety, fault-injection and
  cache-spill paths already have dedicated tests to model after.
- CI also runs the audit, ASan+UBSan and low-memory passes. All three are
  worth running locally (`./dd.ps1 all`) after touching leaf layout, pin
  handling or the transaction paths.
- [docs/testing.md](docs/testing.md) describes the wider strategy (fault
  injection, invariant audits, power-loss simulation, coverage goals).
