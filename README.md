# SpikeDB

A single-file, SIMD-accelerated key-value store built on a disk-backed skip list with LMDB-inspired crash safety.  Supports variable-length byte keys and values with a RocksDB-style API.

## Purpose

SpikeDB is an embedded KV engine designed for workloads where read throughput and low-latency lookups matter most. It stores everything in a single memory-mapped sparse file, avoiding the complexity of LSM-tree compaction or multi-file B-tree management.

## Approach

### Single Sparse File

The database is a single file created as a **sparse file** and mapped entirely into the process's virtual address space via `CreateFileMapping`. The OS only allocates physical disk pages that are actually written to, so a 1 TB virtual extent costs nothing until data arrives.

### Page-Based Layout (v3)

The file is divided into fixed **4 KB pages**:

| Page Range | Role |
|------------|------|
| **Page 0** | Meta Page A — double-buffered root state with CRC32 checksum |
| **Page 1** | Meta Page B — alternate meta page for crash-safe commits |
| **Pages 2-3** | Reader Table — shared mmap slots for multi-process reader coordination |
| **Pages 4+** | Skip Pages and Data Pages |

Internal references use **32-bit Page IDs** instead of 64-bit pointers, doubling the density of routing metadata in SIMD registers.

### Hash-Indexed Skip List

Variable-length keys are hashed to `uint64_t` using a wyhash-inspired hash function.  The 64-bit hashes are stored in Skip Pages for SIMD-accelerated searching.  The actual key and value data is stored separately in Data Pages, referenced by encoded offsets.

On lookup:
1. Hash the search key to `uint64_t`.
2. Descend the skip list using hash-based routing.
3. SIMD-scan the hash array for matches.
4. For each hash match, verify the real key via `memcmp` (handles hash collisions).

This design keeps the SIMD fast path (Bloom check + hash scan) operating on dense `uint64_t` arrays while supporting arbitrary-length keys.

### SIMD Acceleration (Runtime Dispatch)

Three code paths are compiled into every binary. At `spike_db_open()`, CPUID selects the fastest tier available on the running CPU:

| Tier | Register width | Hashes per compare | Bloom filter check |
|------|---------------|-------------------|--------------------|
| **AVX-512** | 512-bit (ZMM) | 8 × 64-bit | Single `_mm512_cmpeq_epi64_mask` |
| **AVX2** | 256-bit (YMM) | 4 × 64-bit | Two-half `_mm256_testz` |
| **Scalar** | 64-bit | 1 × 64-bit | Loop over 8 words |

The dispatch is done via a function-pointer table (`SpikeDB_SimdOps`) stored in the database handle — zero per-call branching overhead.

### SIMD Block Bloom Filter

Each page embeds a **64-byte Bloom filter** (one CPU cache line). Before scanning a page's hash array, the filter is checked with a single vectorised operation. A definite-miss result skips the page entirely, avoiding unnecessary memory-mapped I/O faults.

### Crash Safety (LMDB-Inspired)

SpikeDB v3 uses an LMDB-inspired **double-buffered meta page** strategy for crash-safe commits:

1. **Two meta pages** (pages 0 and 1) each hold a complete snapshot of the database state: root page ID, freelist head, total pages allocated, and data page head.
2. Each meta page carries a **CRC32 checksum** (hardware-accelerated via SSE4.2) covering the meaningful fields. A torn write is detected by checksum mismatch.
3. Each meta page has a **monotonic `txn_id`**. On commit, the database writes to the *older* (inactive) meta page, bumps `txn_id`, recomputes the checksum, and flushes to disk.
4. On open, both meta pages are validated. The one with the higher valid `txn_id` is the current state. If one is corrupt (torn write), the other is used automatically.
5. **Flush barriers** (`FlushViewOfFile` + `FlushFileBuffers`) ensure data pages are durable before the meta page commit point.

**Crash scenarios:**
- Crash *before* meta commit → old meta page is still valid, transaction is rolled back.
- Crash *during* meta write → torn meta page fails CRC32 validation, the other meta page (last committed state) is used.
- Crash *after* meta commit → new state is durable.

### Multi-Process Support

- **Writer serialization**: A **named mutex** (derived from the file path) ensures only one writer across all processes at a time.  The mutex is held for the duration of a put/delete/write-batch operation.
- **Reader coordination**: A **reader table** (pages 2-3, 511 slots) lives in the shared mmap.  Each `spike_db_get()` acquires a slot recording the current `txn_id` and process ID.  Stale slots from crashed processes are detected by PID liveness checks on open.
- **Concurrent readers**: Multiple processes can read simultaneously without blocking each other or blocking the writer.

### WriteBatch

Multiple put/delete operations can be buffered in a `SpikeDB_WriteBatch` and applied in a single `spike_db_write()` call.  The entire batch is applied under one write mutex acquisition and produces a single meta page commit, making it both logically atomic and crash-safe.

### Space Recycling

Freed pages are pushed onto the freelist in the active meta page.  The bump allocator hands out new pages from the tail of the sparse file when the freelist is empty.

## Building

Requires MSVC (Visual Studio 2022 / 2026) with AVX2 support. From a **Developer Command Prompt**:

```
build.bat
```

The resulting `build\test_spike_db.exe` runs 26 tests (CRUD, string keys, WriteBatch, binary data, crash recovery, meta integrity, performance benchmarks) and prints the detected SIMD level.

For AVX-512:

```
build.bat AVX512
```

## API

### Core Operations

```c
SpikeDB_Status spike_db_open(SpikeDB** db, const char* path, uint32_t max_size_gb);
void           spike_db_close(SpikeDB* db);

SpikeDB_Status spike_db_put(SpikeDB* db,
                             const char* key, size_t keylen,
                             const char* val, size_t vallen);

SpikeDB_Status spike_db_get(SpikeDB* db,
                             const char* key, size_t keylen,
                             char** val_out, size_t* vallen_out);

SpikeDB_Status spike_db_delete(SpikeDB* db, const char* key, size_t keylen);

void spike_db_free(void* ptr);  /* free memory returned by get */
```

### WriteBatch

```c
SpikeDB_WriteBatch* spike_db_writebatch_create(void);
void spike_db_writebatch_destroy(SpikeDB_WriteBatch* batch);
void spike_db_writebatch_put(SpikeDB_WriteBatch* batch,
                              const char* key, size_t keylen,
                              const char* val, size_t vallen);
void spike_db_writebatch_delete(SpikeDB_WriteBatch* batch,
                                 const char* key, size_t keylen);
void spike_db_writebatch_clear(SpikeDB_WriteBatch* batch);
int  spike_db_writebatch_count(const SpikeDB_WriteBatch* batch);
SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_WriteBatch* batch);
```

### Example

```c
SpikeDB* db = NULL;
spike_db_open(&db, "my.db", 1);

spike_db_put(db, "user:1", 6, "Alice", 5);

char* val = NULL;
size_t vlen = 0;
if (spike_db_get(db, "user:1", 6, &val, &vlen) == SPIKEDB_OK) {
    printf("Got: %.*s\n", (int)vlen, val);
    spike_db_free(val);
}

/* Batch writes — single commit, crash-safe */
SpikeDB_WriteBatch* batch = spike_db_writebatch_create();
spike_db_writebatch_put(batch, "user:2", 6, "Bob", 3);
spike_db_writebatch_put(batch, "user:3", 6, "Carol", 5);
spike_db_writebatch_delete(batch, "user:1", 6);
spike_db_write(db, batch);
spike_db_writebatch_destroy(batch);

spike_db_close(db);
```

### Return Codes

| Code | Value | Meaning |
|------|-------|---------|
| `SPIKEDB_OK` | 0 | Success |
| `SPIKEDB_NOT_FOUND` | -1 | Key does not exist |
| `SPIKEDB_ERROR` | -2 | General error (record too large, allocation failure) |
| `SPIKEDB_FULL` | -3 | Database file is full |

## License

Unlicensed / public domain — spike away.
