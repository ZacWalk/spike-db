/*============================================================================
 * spike_db.h  —  SpikeDB v5 (skip-list, financial time-series)
 *
 * Composite primary key: (symbol_u64, time_u64).
 * One skip list of leaf pages per symbol; leaves form a doubly-linked list.
 * 64 KB pages, page cache (no mmap), CoW + double-buffered meta for crash
 * safety, atomic batch writes.
 *
 * See docs/skiplist-redesign-plan.md for the full design.
 *
 *----------------------------------------------------------------------------
 * Concurrency model
 *----------------------------------------------------------------------------
 *
 * - **NOT thread-safe within a single handle.** A `SpikeDB*` is owned by one
 *   thread at a time. Concurrent calls on the same handle from different
 *   threads will corrupt the page cache and produce undefined behavior.
 *   Either give each thread its own handle (multiple opens of the same
 *   file are supported) or serialize access externally.
 *
 * - **Multi-handle and multi-process safe.** Multiple `SpikeDB*` instances
 *   (in the same process or in different processes) may operate on the
 *   same file concurrently. Coordination uses an OS file range lock:
 *     - Readers (`get`, `scan`, `min/max/count`) acquire a SHARED lock.
 *     - Writers (`write`, `truncate_before`) acquire an EXCLUSIVE lock.
 *   Iterators hold the shared lock for their full lifetime, which
 *   serializes writers against any open iterator. Keep iterator
 *   lifetimes short.
 *
 * - **Endianness:** on-disk format is little-endian only. The library
 *   assumes the host is little-endian (x86-64, ARM64). A file written on
 *   a little-endian host is not portable to a big-endian one.
 *
 *----------------------------------------------------------------------------
 * Crash safety
 *----------------------------------------------------------------------------
 *
 * Every `spike_db_write` and `spike_db_truncate_before` is atomic and
 * durable across crashes: copy-on-write data pages are flushed first,
 * then the inactive meta page is rewritten with a new transaction ID
 * and CRC32, then flushed. On open, the meta with the highest valid
 * txn_id wins; a half-completed transaction is invisible.
 *============================================================================*/

#ifndef SPIKE_DB_H
#define SPIKE_DB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define SPIKEDB_PAGE_SIZE       65536u
#define SPIKEDB_MAGIC           0x5350494B45444205ULL   /* "SPIKEDB\x05" */
#define SPIKEDB_INVALID_PAGE    0xFFFFFFFFu
#define SPIKEDB_MAX_LEVEL       16

/* Reserved file layout (page indices) */
#define SPIKEDB_META_A_PAGE     0u
#define SPIKEDB_META_B_PAGE     1u
#define SPIKEDB_SYMDIR_START    2u
#define SPIKEDB_SYMDIR_PAGES    16u    /* 16 * 64KB = 1 MiB ≈ 65k symbol slots */
#define SPIKEDB_RESERVED_PAGES  (SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES)

/* Open flags */
#define SPIKEDB_OPEN_READONLY   0x1u   /* file must already exist */

/*============================================================================
 * Public types
 *============================================================================*/

typedef enum {
    SPIKEDB_OK         =  0,
    SPIKEDB_NOT_FOUND  = -1,
    SPIKEDB_ERROR      = -2,
    /* Returned only in the rare case that every cache slot is pinned and
     * therefore no eviction victim can be found. The cache normally spills
     * dirty pages to disk to make room (see SpikeDB_Stats::cache_spills),
     * so this typically indicates a programmer error such as leaked page
     * pins (e.g. an iterator never closed). The transaction is rolled
     * back cleanly. */
    SPIKEDB_FULL       = -3,
    SPIKEDB_CORRUPT    = -4,
    SPIKEDB_INVAL      = -5,
} SpikeDB_Status;

typedef struct SpikeDB        SpikeDB;
typedef struct SpikeDB_Batch  SpikeDB_Batch;
typedef struct SpikeDB_Iter   SpikeDB_Iter;

/*============================================================================
 * API — open / close
 *
 * On open:
 *   - `cache_pages_64k` is the page-cache capacity in 64 KB pages. Values
 *     below 16 are silently raised to 16 (1 MiB).
 *   - With `SPIKEDB_OPEN_READONLY`, the file must already exist;
 *     SPIKEDB_ERROR is returned otherwise. Without the flag, a missing
 *     file is created and freshly formatted.
 *
 * On close:
 *   - If a transaction is in progress (started by `spike_db_batch_put` /
 *     `spike_db_write`), it is rolled back and any unwritten data is lost.
 *     Always finish writes before calling `spike_db_close`.
 *============================================================================*/

SpikeDB_Status spike_db_open (SpikeDB** out, const char* path,
                              uint32_t cache_pages_64k, uint32_t flags);
void           spike_db_close(SpikeDB* db);

/*============================================================================
 * API — point lookup
 *============================================================================*/

/* Returns a malloc'd buffer in *value_out (free with spike_db_free).
 * Returns SPIKEDB_NOT_FOUND if the (symbol, time) pair is absent. */
SpikeDB_Status spike_db_get(SpikeDB* db, uint64_t symbol, uint64_t time,
                            void** value_out, size_t* len_out);

void spike_db_free(void* ptr);

/*============================================================================
 * API — atomic batch insert
 *
 * Records may be added in any order; the engine sorts internally before
 * applying. The whole batch either commits atomically or leaves the DB
 * unchanged.
 *
 * Constraints:
 *   - Maximum value length per record is 65000 bytes; longer values are
 *     rejected with SPIKEDB_INVAL by `spike_db_batch_put`.
 *   - Within a single batch, every (symbol, time) pair must be unique.
 *     A duplicate causes `spike_db_write` to fail atomically (no records
 *     applied) and return SPIKEDB_INVAL.
 *
 * Reuse:
 *   - `spike_db_write` does NOT clear the batch. To reuse the batch for
 *     another commit, call `spike_db_batch_clear` first; otherwise the
 *     same records will be written again (which will then fail as
 *     duplicates).
 *============================================================================*/

SpikeDB_Batch* spike_db_batch_create(void);
void           spike_db_batch_destroy(SpikeDB_Batch* batch);
void           spike_db_batch_clear  (SpikeDB_Batch* batch);
size_t         spike_db_batch_count  (const SpikeDB_Batch* batch);

SpikeDB_Status spike_db_batch_put(SpikeDB_Batch* batch,
                                  uint64_t symbol, uint64_t time,
                                  const void* value, size_t len);

SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_Batch* batch);

/*============================================================================
 * API — range scan iterator
 *
 *   for (SpikeDB_Iter* it = spike_db_scan(db, sym, lo, hi);
 *        spike_db_iter_next(it, &t, &val, &len); ) { ... }
 *   spike_db_iter_close(it);
 *
 * The pointer returned in *val_out is owned by the iterator and is valid
 * only until the next iter_next() / iter_close() call.
 *============================================================================*/

SpikeDB_Iter* spike_db_scan(SpikeDB* db, uint64_t symbol,
                            uint64_t time_lo, uint64_t time_hi);
bool          spike_db_iter_next (SpikeDB_Iter* it,
                                  uint64_t* time_out,
                                  const void** value_out, size_t* len_out);
void          spike_db_iter_close(SpikeDB_Iter* it);

/*============================================================================
 * API — fast metadata queries (single cached page read)
 *
 * On any non-OK return, `*out` is set to 0.
 *============================================================================*/

SpikeDB_Status spike_db_max_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_min_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_count   (SpikeDB* db, uint64_t symbol, uint64_t* out);

/*============================================================================
 * API — retention
 *
 * Atomically drop every record for `symbol` whose time is strictly less
 * than `time_exclusive`. Frees whole leaf pages whose max_time falls below
 * the cutoff; partially-overlapping leaves are left in place.
 *============================================================================*/

SpikeDB_Status spike_db_truncate_before(SpikeDB* db, uint64_t symbol,
                                        uint64_t time_exclusive);

/*============================================================================
 * Stats (diagnostic only — counters are not synchronized; values may be
 * inconsistent if read while other operations are in flight on different
 * handles).
 *============================================================================*/

typedef struct SpikeDB_Stats {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t txn_id;
    uint32_t cache_capacity;
    uint32_t cache_used;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_spills;   /* dirty pages written out mid-txn to free cache */
    uint64_t symbol_count;
} SpikeDB_Stats;

void spike_db_stats(SpikeDB* db, SpikeDB_Stats* out);

#ifdef __cplusplus
}
#endif

#endif /* SPIKE_DB_H */
