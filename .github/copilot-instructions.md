# Copilot Instructions for SpikeDB

Important: place stest output and temporary files in the `tmp/` directory, not the project root. This keeps the repo clean and avoids accidentally committing test artifacts.

## Project Overview

SpikeDB is a single-file, SIMD-accelerated key-value store written in C. It uses **SHIP** (SIMD Hash-Indexed Pages) — an extendible hash directory with SIMD-scanned leaf pages, Bloom filters, and LMDB-inspired crash safety. The entire database is implemented in two files: `src/spike_db.c` (implementation) and `src/spike_db.h` (public header).

## Language & Toolchain

- **Language**: C (C11), single-header + single-implementation file
- **Windows**: MSVC (Visual Studio 2026/2025/2022), built via `build.ps1` or `run_tests.ps1`
- **Linux**: GCC with `-mavx2 -msse4.2`
- **SIMD**: Runtime CPUID dispatch across AVX-512, AVX2, and scalar fallbacks
- **No external dependencies** — everything is self-contained

## Build & Test

```powershell
# Build only
./build.ps1 -Arch AVX2

# Build and run tests
./run_tests.ps1 -Arch AVX2
```

Output goes to `build/`. The test binary is `build/test_spike_db.exe` (Windows) or `build/test_spike_db` (Linux).

## Architecture

- **Page size**: 4 KB throughout (leaf pages, data pages, meta pages, directory pages)
- **Extendible hash directory**: Top `global_depth` bits of a 64-bit wyhash select a directory slot pointing to a leaf page
- **Leaf pages**: Hold up to 64 sorted hash entries, SIMD-scanned for lookups
- **Bloom filters**: 64-byte cache-line-aligned, 8 hash probes per key for fast negative lookups
- **Data pages**: Append-only variable-length records (`[key_len:u32][val_len:u32][key][val]`), track `live_bytes`/`live_records`, returned to freelist when fully dead
- **Crash safety**: Double-buffered meta pages (pages 0–1) with CRC32 checksums, commit flips the active meta page after `fdatasync`/`FlushFileBuffers`
- **Page cache**: LRU with pin/unpin ref-counting, O(1) hash-table slot lookup, copy-on-write dirty tracking
- **WriteBatch**: Atomic multi-key commits with rollback on failure (snapshots allocated pages)

## File Layout (on disk)

| Pages   | Purpose                                      |
|---------|----------------------------------------------|
| 0–1     | Double-buffered meta pages                   |
| 2–3     | Reader table (shared memory for concurrency) |
| 4–259   | Extendible hash directory (256 pages max)    |
| 260+    | Leaf pages and data pages                    |

## Coding Conventions

- Prefix all public symbols with `spike_db_` or `SpikeDB_`
- Internal/static functions use `snake_case` without the `spike_db_` prefix
- Constants and macros use `SPIKEDB_` prefix with `UPPER_SNAKE_CASE`
- Use `#pragma pack(push, 1)` for on-disk structures
- Platform-specific code is guarded with `#ifdef _WIN32` / `#ifdef _MSC_VER`
- SIMD intrinsics are isolated behind the `SpikeDB_SimdOps` dispatch table
- All page access goes through pin/unpin helpers (`page_ref_pin` / `page_ref_unpin`)
- Return `SpikeDB_Status` codes: `SPIKEDB_OK` (0), `SPIKEDB_NOT_FOUND` (-1), `SPIKEDB_ERROR` (-2), `SPIKEDB_FULL` (-3)

## Testing

- Tests are in `src/test_spike_db.c` using a minimal built-in test framework (no external test library)
- Test macros: `CHECK(cond, fmt, ...)` and `CHECK_CONTINUE(cond, counter, fmt, ...)`
- Each test function cleans up its database file (`TEST_DB_PATH = "test_spike_db.dat"`)
- Default cache size for tests: 2048 pages (8192 in exclusive mode)
