/*============================================================================
 * spike_db.h  —  SpikeDB v7 (skip-list, financial time-series)
 *
 * Composite primary key: (symbol_u64, time_u64, seq_u32).
 * One skip list of leaf pages per symbol; leaves form a doubly-linked list.
 * 64 KB pages, page cache (no mmap), CoW + double-buffered meta for crash
 * safety, atomic batch writes.
 *
 * `seq` is a tiebreaker for events that share a timestamp — routine in
 * market data, where one aggressive order prints several fills under a
 * single exchange nanosecond. Series that are naturally unique per
 * timestamp (bars, snapshots) should pass seq = 0 and can use the
 * seq-less convenience wrappers throughout.
 *
 * See docs/design.md for the full design.
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
 *   `spike_db_scan` holds the shared lock for the iterator's full
 *   lifetime, which serializes writers against any open iterator; scan
 *   with `SPIKEDB_SCAN_NONBLOCKING` to drop the lock between chunks
 *   instead. See the iterator section for the isolation trade.
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
#define SPIKEDB_MAGIC           0x5350494B45444207ULL   /* "SPIKEDB\x07" */
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

/* Per-record flags for spike_db_batch_put_ex */
#define SPIKEDB_PUT_OVERWRITE   0x1u   /* replace an existing key instead of failing */

/* Flags for spike_db_write_ex */
#define SPIKEDB_WRITE_NOSYNC    0x1u   /* durable to the OS, not to the platter */

/* Flags for spike_db_scan_ex */
#define SPIKEDB_SCAN_NONBLOCKING 0x1u  /* release the reader lock between chunks */
#define SPIKEDB_SCAN_REVERSE     0x2u  /* walk newest to oldest */

/* Capacity of the transactional user-metadata area (see batch_put_meta). */
#define SPIKEDB_META_CAPACITY   4096u

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
 *   - **Close every iterator first.** A `SpikeDB_Iter` borrows the handle
 *     and holds a file lock for its lifetime; closing the handle while one
 *     is open leaves the iterator dangling and the lock held, and any later
 *     call on that iterator — including `spike_db_iter_close` — is
 *     undefined behavior. The library does not track open iterators.
 *============================================================================*/

SpikeDB_Status spike_db_open (SpikeDB** out, const char* path,
                              uint32_t cache_pages_64k, uint32_t flags);
void           spike_db_close(SpikeDB* db);

/* Extensible form of spike_db_open. Set `struct_size = sizeof(SpikeDB_Options)`
 * and zero the rest; fields added in future versions default to the v7
 * behavior when the caller passes a smaller struct. */
typedef struct SpikeDB_Options {
    uint32_t struct_size;
    uint32_t cache_pages_64k;
    uint32_t flags;
} SpikeDB_Options;

SpikeDB_Status spike_db_open_ex(SpikeDB** out, const char* path,
                                const SpikeDB_Options* opts);

/* Library version as MAJOR*10000 + MINOR*100 + PATCH, and the on-disk
 * format version (6). A file written by a different format version cannot
 * be opened. */
uint32_t spike_db_version(void);
uint32_t spike_db_format_version(void);

/*============================================================================
 * API — point lookup
 *
 * All three return a malloc'd buffer in *value_out (free with
 * spike_db_free) and SPIKEDB_NOT_FOUND when no record qualifies.
 *============================================================================*/

/* Exact match on (symbol, time, seq). `spike_db_get` implies seq = 0. */
SpikeDB_Status spike_db_get(SpikeDB* db, uint64_t symbol, uint64_t time,
                            void** value_out, size_t* len_out);
SpikeDB_Status spike_db_get_seq(SpikeDB* db, uint64_t symbol, uint64_t time,
                                uint32_t seq,
                                void** value_out, size_t* len_out);

/* As-of lookup: the latest record at or before `time` (highest seq at that
 * timestamp). This is the point-in-time primitive — last trade as of T,
 * prevailing quote as of T — and is what feature pipelines need to avoid
 * lookahead bias. `time_out` / `seq_out` may be NULL. */
SpikeDB_Status spike_db_get_le(SpikeDB* db, uint64_t symbol, uint64_t time,
                               uint64_t* time_out, uint32_t* seq_out,
                               void** value_out, size_t* len_out);

/* The earliest record at or after `time` (lowest seq at that timestamp). */
SpikeDB_Status spike_db_get_ge(SpikeDB* db, uint64_t symbol, uint64_t time,
                               uint64_t* time_out, uint32_t* seq_out,
                               void** value_out, size_t* len_out);

void spike_db_free(void* ptr);

/*============================================================================
 * API — fixed-width symbols
 *
 * Financial payloads are overwhelmingly homogeneous POD structs — a tick,
 * an OHLCV bar. Declaring that up front switches the symbol's leaves to a
 * columnar layout (`times[] | seqs[] | payloads[]`) with no per-record slot
 * directory, which:
 *   - stores 10-25% more records per page, depending on record size;
 *   - makes `spike_db_read_range` one memcpy per leaf straight into the
 *     caller's array, instead of a copy per record.
 *
 * Must be called before the symbol has any records; changing the layout of
 * a populated symbol, or re-declaring a different size, returns
 * SPIKEDB_INVAL. Repeating the same declaration is harmless. Once
 * declared, every value written for that symbol must be exactly
 * `record_size` bytes.
 *============================================================================*/

SpikeDB_Status spike_db_symbol_define(SpikeDB* db, uint64_t symbol,
                                      uint32_t record_size);

/* Copy the payloads for `[time_lo, time_hi]` into `dst` as a packed array
 * of `record_size`-byte records. Fixed-width symbols only.
 *
 * Pass dst = NULL and dst_records = 0 to count the range without copying.
 * Returns SPIKEDB_FULL when more records matched than `dst` could hold, in
 * which case `dst` is filled and *count_out is dst_records. */
SpikeDB_Status spike_db_read_range(SpikeDB* db, uint64_t symbol,
                                   uint64_t time_lo, uint64_t time_hi,
                                   void* dst, size_t dst_records,
                                   size_t* count_out);

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
 *   - Within a single batch, every (symbol, time, seq) triple must be
 *     unique unless SPIKEDB_PUT_OVERWRITE is used. A duplicate causes
 *     `spike_db_write` to fail atomically (no records applied) and return
 *     SPIKEDB_INVAL.
 *   - Entries touching the same key are applied in submission order, so
 *     with SPIKEDB_PUT_OVERWRITE the last one wins.
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

/* `spike_db_batch_put` implies seq = 0. */
SpikeDB_Status spike_db_batch_put(SpikeDB_Batch* batch,
                                  uint64_t symbol, uint64_t time,
                                  const void* value, size_t len);
SpikeDB_Status spike_db_batch_put_seq(SpikeDB_Batch* batch,
                                      uint64_t symbol, uint64_t time,
                                      uint32_t seq,
                                      const void* value, size_t len);

/* `flags` is a bitwise-or of SPIKEDB_PUT_*. With SPIKEDB_PUT_OVERWRITE an
 * existing record is replaced rather than rejected — the form needed for
 * vendor restatements, busted trades, and for replaying a batch
 * idempotently after an ambiguous crash. */
SpikeDB_Status spike_db_batch_put_ex(SpikeDB_Batch* batch,
                                     uint64_t symbol, uint64_t time,
                                     uint32_t seq,
                                     const void* value, size_t len,
                                     uint32_t flags);

/* Queue a delete. Removing a key that is not present is not an error. */
SpikeDB_Status spike_db_batch_del(SpikeDB_Batch* batch,
                                  uint64_t symbol, uint64_t time, uint32_t seq);

/* Durably record the ingest position (exchange sequence, Kafka offset,
 * file byte position) in the *same* transaction as the data, so a restart
 * cannot produce a gap or a double-apply. Keys are NUL-terminated strings;
 * the encoded key/value set must fit in SPIKEDB_META_CAPACITY bytes, which
 * `spike_db_write` enforces by returning SPIKEDB_FULL. Writing a key that
 * already exists replaces it; passing len = 0 erases it. */
SpikeDB_Status spike_db_batch_put_meta(SpikeDB_Batch* batch, const char* key,
                                       const void* value, size_t len);

/* Reads a key written by spike_db_batch_put_meta. *value_out is malloc'd
 * (free with spike_db_free) and NUL-terminated for convenience; *len_out
 * excludes that terminator. */
SpikeDB_Status spike_db_get_meta(SpikeDB* db, const char* key,
                                 void** value_out, size_t* len_out);

SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_Batch* batch);

/* `spike_db_write` fsyncs twice per commit, which is right for a batch
 * loader and wrong for a tick ingester committing every few milliseconds.
 * With SPIKEDB_WRITE_NOSYNC the commit is ordered and atomic but only
 * durable to the OS page cache, so a power loss can lose recent commits
 * while leaving the file structurally intact. That is a normal trade in
 * trading systems — the exchange is the system of record and the ingest
 * cursor (see spike_db_batch_put_meta) says where to resume — but it has
 * to be an explicit choice. Call `spike_db_sync` to flush on your own
 * schedule. */
SpikeDB_Status spike_db_write_ex(SpikeDB* db, SpikeDB_Batch* batch, uint32_t flags);
SpikeDB_Status spike_db_sync(SpikeDB* db);

/*============================================================================
 * API — range scan iterator
 *
 *   for (SpikeDB_Iter* it = spike_db_scan(db, sym, lo, hi);
 *        spike_db_iter_next(it, &t, &val, &len); ) { ... }
 *   spike_db_iter_close(it);
 *
 * The range is inclusive on both ends and covers every seq at each
 * timestamp. Records are yielded in (time, seq) order.
 *
 * The pointer returned in *val_out is owned by the iterator and is valid
 * only until the next iter_next() / iter_close() call.
 *
 * An iterator borrows its `SpikeDB*` and must be closed before it — see
 * the close notes above.
 *
 *----------------------------------------------------------------------------
 * Isolation
 *----------------------------------------------------------------------------
 *
 * `spike_db_scan` holds the shared reader lock for the iterator's whole
 * lifetime. That gives a stable view, but it also blocks every writer
 * until the iterator is closed — an hours-long scan will stall the
 * ingester for hours. Use it only for short scans.
 *
 * `SPIKEDB_SCAN_NONBLOCKING` instead reads a chunk of records at a time
 * and drops the lock in between, so a writer waits at most for one chunk.
 * The trade is isolation: the scan is read-committed, not a snapshot. It
 * always advances in key order and never repeats a record, but records
 * committed mid-scan ahead of the cursor are visible, records inserted
 * behind it are not, and a record can disappear before it is reached.
 * Once the range is exhausted the iterator stays finished; it does not
 * become a live tail.
 *============================================================================*/

SpikeDB_Iter* spike_db_scan(SpikeDB* db, uint64_t symbol,
                            uint64_t time_lo, uint64_t time_hi);
SpikeDB_Iter* spike_db_scan_ex(SpikeDB* db, uint64_t symbol,
                               uint64_t time_lo, uint64_t time_hi,
                               uint32_t flags);
bool          spike_db_iter_next (SpikeDB_Iter* it,
                                  uint64_t* time_out,
                                  const void** value_out, size_t* len_out);
bool          spike_db_iter_next_seq(SpikeDB_Iter* it,
                                     uint64_t* time_out, uint32_t* seq_out,
                                     const void** value_out, size_t* len_out);

/* Bulk form for replay loops: fills up to `max` records and returns how
 * many were written. `value` points into the iterator's buffer and stays
 * valid until the next call on this iterator. A short non-zero return
 * only means the current chunk ran out — keep calling until it returns 0.
 * Works for both single- and multi-symbol iterators. */
typedef struct SpikeDB_Rec {
    uint64_t    symbol;
    uint64_t    time;
    uint32_t    seq;
    uint32_t    len;
    const void* value;
} SpikeDB_Rec;

size_t        spike_db_iter_next_batch(SpikeDB_Iter* it,
                                       SpikeDB_Rec* out, size_t max);

/* Reposition a single-symbol iterator at `(time, seq)` and resume from
 * there in its current direction, discarding anything buffered. Cheaper
 * than closing and reopening because the lock and buffer are reused.
 * Not supported on merged iterators. */
SpikeDB_Status spike_db_iter_seek(SpikeDB_Iter* it, uint64_t time, uint32_t seq);

void          spike_db_iter_close(SpikeDB_Iter* it);

/*============================================================================
 * API — merged multi-symbol replay
 *
 * Yields the records of several symbols as one stream in (time, seq)
 * order, with the symbol id as a tiebreaker. This is what a backtest or a
 * live event loop consumes; doing the same merge in caller code means one
 * iterator per symbol, each holding its own lock and page pins, which is
 * exactly the pattern that exhausts the page cache.
 *
 * `symbols` is copied, so the caller's array need not outlive the
 * iterator. Duplicate ids are not filtered. `flags` takes the same
 * SPIKEDB_SCAN_* values as `spike_db_scan_ex`; note that in non-blocking
 * mode every commit observed mid-scan costs one re-descent per symbol, so
 * it suits watchlists of tens to hundreds rather than thousands.
 *
 * The merged stream never moves backwards in time: after a concurrent
 * commit, every cursor resumes from the last key the merge emitted, so
 * records that landed behind it are skipped rather than emitted late.
 *============================================================================*/

SpikeDB_Iter* spike_db_scan_multi(SpikeDB* db,
                                  const uint64_t* symbols, size_t count,
                                  uint64_t time_lo, uint64_t time_hi,
                                  uint32_t flags);

bool spike_db_iter_next_multi(SpikeDB_Iter* it,
                              uint64_t* symbol_out, uint64_t* time_out,
                              uint32_t* seq_out,
                              const void** value_out, size_t* len_out);

/*============================================================================
 * API — fast metadata queries (single cached page read)
 *
 * On any non-OK return, `*out` is set to 0.
 *============================================================================*/

SpikeDB_Status spike_db_max_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_min_time(SpikeDB* db, uint64_t symbol, uint64_t* out);
SpikeDB_Status spike_db_count   (SpikeDB* db, uint64_t symbol, uint64_t* out);

/* Everything the three calls above return, for one lock acquisition and
 * one page read instead of three. `record_count == 0` means the symbol
 * exists but is empty; SPIKEDB_NOT_FOUND means it was never written. */
typedef struct SpikeDB_SymbolInfo {
    uint64_t min_time;      /* 0 when empty */
    uint64_t max_time;      /* 0 when empty */
    uint64_t record_count;
    uint64_t leaf_count;
    uint32_t record_size;   /* 0 = variable-width */
} SpikeDB_SymbolInfo;

SpikeDB_Status spike_db_symbol_info(SpikeDB* db, uint64_t symbol,
                                    SpikeDB_SymbolInfo* out);

/* Batched "what is the latest tick?" for a whole watchlist under a single
 * lock acquisition. Symbols that are absent or empty get 0. */
SpikeDB_Status spike_db_max_times(SpikeDB* db, const uint64_t* symbols,
                                  uint64_t* times_out, size_t count);

/* Monotonic commit counter. A poller that caches this can skip all
 * per-symbol work when nothing at all has been committed since last time.
 * Costs one meta refresh and no page reads. */
SpikeDB_Status spike_db_txn_id(SpikeDB* db, uint64_t* out);

/* Enumerate the symbols present in the file. Writes up to `cap` ids into
 * `out` and always reports the true total in *count_out, so a caller can
 * size a buffer with a first call passing cap = 0. */
SpikeDB_Status spike_db_list_symbols(SpikeDB* db, uint64_t* out, size_t cap,
                                     size_t* count_out);

/* Block until the file's txn id differs from `last_seen`, or `timeout_ms`
 * elapses (0 polls once). Returns SPIKEDB_NOT_FOUND on timeout. This is an
 * adaptive backoff poll rather than an OS wait primitive, but it keeps the
 * spin in one place instead of in every strategy's loop. */
SpikeDB_Status spike_db_wait_for_txn(SpikeDB* db, uint64_t last_seen,
                                     uint32_t timeout_ms, uint64_t* out);

/* Warm the page cache for a range so a strategy's first scan is not
 * I/O bound. Stops early once it has touched as much as the cache holds. */
SpikeDB_Status spike_db_prefetch(SpikeDB* db, uint64_t symbol,
                                 uint64_t time_lo, uint64_t time_hi);

/* Consistent online copy of the whole file, for archiving or for cloning
 * production data to a research box. Holds the reader lock for the
 * duration, so writers wait; take backups off a replica if that matters. */
SpikeDB_Status spike_db_backup(SpikeDB* db, const char* dest_path);

/* Remove a symbol entirely: every record, its leaves, its index nodes and
 * its directory entry. Unlike deleting all records, this also releases the
 * root page and the directory slot. */
SpikeDB_Status spike_db_symbol_drop(SpikeDB* db, uint64_t symbol);

/*============================================================================
 * API — retention
 *
 * Atomically drop every record for `symbol` whose time is strictly less
 * than `time_exclusive`. Frees whole leaf pages whose max_time falls below
 * the cutoff; partially-overlapping leaves are left in place.
 *============================================================================*/

SpikeDB_Status spike_db_truncate_before(SpikeDB* db, uint64_t symbol,
                                        uint64_t time_exclusive);

/* Atomically drop every record for `symbol` in the inclusive time range,
 * across all seq values. Leaves emptied by the operation are freed. */
SpikeDB_Status spike_db_delete_range(SpikeDB* db, uint64_t symbol,
                                     uint64_t time_lo, uint64_t time_hi);

/*============================================================================
 * Diagnostics
 *
 * `SPIKEDB_ERROR` on its own is not actionable in production — it covers
 * ENOSPC, EACCES, EIO and lock failures alike. `spike_db_last_error`
 * reports the OS error and the page involved for the most recent failing
 * call on this handle. It describes a live handle only; a failed
 * `spike_db_open` has no handle to report through.
 *============================================================================*/

typedef struct SpikeDB_Error {
    SpikeDB_Status status;
    int            os_error;    /* errno / GetLastError(), 0 if not applicable */
    uint64_t       page;        /* page involved, or SPIKEDB_INVALID_PAGE */
    char           message[128];
} SpikeDB_Error;

const char* spike_db_strerror(SpikeDB_Status status);
void        spike_db_last_error(SpikeDB* db, SpikeDB_Error* out);

/*============================================================================
 * API — integrity check
 *
 * Every page carries a CRC-32C, so ordinary reads already reject a torn or
 * bit-rotted page with SPIKEDB_CORRUPT. `spike_db_verify` additionally
 * walks the whole file — every symbol's leaf chain, slot ordering, value
 * extents, cached counters, skip-list levels and the free list — so an
 * operator can answer "is this file trustworthy?" before a training run or
 * an archive, rather than discovering it from a wrong price.
 *
 * Returns SPIKEDB_OK when clean and SPIKEDB_CORRUPT when `errors` is
 * non-zero; `first_error` describes the first problem found.
 *============================================================================*/

typedef struct SpikeDB_VerifyReport {
    uint64_t symbols;
    uint64_t leaves;
    uint64_t records;
    uint64_t free_pages;
    uint64_t errors;
    char     first_error[192];
} SpikeDB_VerifyReport;

SpikeDB_Status spike_db_verify(SpikeDB* db, SpikeDB_VerifyReport* out);

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
