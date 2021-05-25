#ifndef SPIKE_DB_H
#define SPIKE_DB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Platform alignment macros
 *============================================================================*/

#ifdef _MSC_VER
#define SPIKEDB_ALIGN_PRE(n)  __declspec(align(n))
#define SPIKEDB_ALIGN_POST(n)
#else
#define SPIKEDB_ALIGN_PRE(n)
#define SPIKEDB_ALIGN_POST(n) __attribute__((aligned(n)))
#endif

/*============================================================================
 * Constants
 *============================================================================*/

#define SPIKEDB_PAGE_SIZE           4096
#define SPIKEDB_SIMD_ALIGN          64
#define SPIKEDB_MAX_LEVEL           16
#define SPIKEDB_KEYS_PER_PAGE       64
#define SPIKEDB_MAGIC               0x5350494B45444203ULL  /* "SPIKEDB\x03" v3 */
#define SPIKEDB_INVALID_PAGE        0xFFFFFFFFU

/* Data Page constants */
#define SPIKEDB_DATA_PAGE_HDR_SIZE  16
#define SPIKEDB_DATA_PAGE_PAYLOAD   (SPIKEDB_PAGE_SIZE - SPIKEDB_DATA_PAGE_HDR_SIZE)
#define SPIKEDB_MAX_RECORD_SIZE     (SPIKEDB_DATA_PAGE_PAYLOAD - 8)  /* 8 = klen+vlen */

/* Open flags (bitfield) */
#define SPIKEDB_OPEN_EXCLUSIVE      0x1   /* Single-process mode: skip mutex and reader table */

/*============================================================================
 * Reserved file layout (LMDB-inspired):
 *
 *   Page 0 — Meta Page A   (double-buffered with page 1)
 *   Page 1 — Meta Page B
 *   Pages 2-3 — Reader Table  (shared mmap, multi-process safe)
 *   Pages 4+ — Skip Pages and Data Pages
 *
 * The double-buffered meta pages provide crash-safe root pointer updates:
 * on commit, the OLDER meta page is overwritten with the new state.  If the
 * process crashes mid-write, the other meta page is still valid.
 *============================================================================*/
#define SPIKEDB_RESERVED_PAGES      4   /* meta A, meta B, reader tbl x 2 */

/*============================================================================
 * SIMD capability levels (detected at runtime via CPUID)
 *============================================================================*/

typedef enum {
    SPIKEDB_SIMD_SCALAR = 0,
    SPIKEDB_SIMD_AVX2   = 1,
    SPIKEDB_SIMD_AVX512 = 2,
} SpikeDB_SimdLevel;

/*============================================================================
 * Data Structures
 *============================================================================*/

/* 64-byte cache-line-aligned Bloom Filter Block. */
typedef SPIKEDB_ALIGN_PRE(64) struct SpikeDB_BloomBlock {
    uint64_t words[8];
} SPIKEDB_ALIGN_POST(64) SpikeDB_BloomBlock;

/* 4 KB Skip Page — the core index node of the SIMD skip list.
 *
 * Each page holds up to SPIKEDB_KEYS_PER_PAGE entries sorted by the 64-bit
 * hash of the original key.  hashes[] is SIMD-scanned; data_offsets[] point
 * to Data Pages holding the real key/value bytes. */
#pragma pack(push, 1)
typedef SPIKEDB_ALIGN_PRE(4096) struct SpikeDB_Page {
    uint32_t           next_page_ids[SPIKEDB_MAX_LEVEL];
    SpikeDB_BloomBlock bloom;
    uint16_t           key_count;
    uint16_t           level;
    uint32_t           epoch_freed;
    uint64_t           hashes[SPIKEDB_KEYS_PER_PAGE];
    uint64_t           data_offsets[SPIKEDB_KEYS_PER_PAGE];
    uint8_t            _pad[SPIKEDB_PAGE_SIZE
                            - (SPIKEDB_MAX_LEVEL * 4)
                            - sizeof(SpikeDB_BloomBlock)
                            - 8
                            - (SPIKEDB_KEYS_PER_PAGE * 8 * 2)];
} SPIKEDB_ALIGN_POST(4096) SpikeDB_Page;
#pragma pack(pop)

/* Data Page — stores variable-length key/value records.
 *   [uint32_t key_len][uint32_t val_len][key bytes...][value bytes...]
 * Records are append-only; stale records are dead space. */
typedef struct SpikeDB_DataPageHeader {
    uint32_t used_bytes;
    uint32_t _reserved[3];
} SpikeDB_DataPageHeader;

/*============================================================================
 * Meta Page — double-buffered at pages 0 and 1  (LMDB-inspired)
 *
 * Each Meta Page is a full 4 KB page containing the database state snapshot.
 * On commit, the OLDER of the two meta pages is overwritten with the new
 * state (txn_id incremented).  A CRC32 checksum covers the meaningful fields
 * so that a torn write can be detected on recovery.
 *
 * On open, both meta pages are read and validated.  The one with the higher
 * valid txn_id is the active state.  If both are invalid (new file), the
 * database is initialized fresh.
 *============================================================================*/
typedef struct SpikeDB_MetaPage {
    uint64_t magic;                     /* SPIKEDB_MAGIC                     */
    uint64_t txn_id;                    /* monotonic transaction counter     */
    uint32_t root_page_id;             /* root of the skip list             */
    uint32_t freelist_head;            /* head of freed-page list           */
    uint64_t total_pages_allocated;    /* bump-allocator watermark          */
    uint32_t data_page_head;           /* active Data Page for writes       */
    uint32_t checksum;                 /* CRC32 of bytes 0..35 (above)      */
    uint8_t  _pad[SPIKEDB_PAGE_SIZE - 40];
} SpikeDB_MetaPage;

/*============================================================================
 * Reader Table — pages 2-3 (8 KB, shared via mmap)
 *
 * Provides multi-process reader coordination (LMDB-style).  Each reader
 * acquires a slot and records the txn_id it is reading.  The writer checks
 * the minimum active txn_id before reclaiming freed pages, ensuring that
 * no reader is still traversing old data.
 *
 * Slot lifecycle:
 *   1. spike_db_get()  -> acquire slot (txn_id = current active meta's txn_id)
 *   2. perform read
 *   3. release slot (txn_id = 0)
 *
 * Stale-process detection: if a process crashes while holding a reader slot,
 * the recorded pid will no longer be running.  On the next open or write, the
 * database can clear stale slots by checking pid liveness.
 *============================================================================*/
#define SPIKEDB_READER_TABLE_PAGES  2
#define SPIKEDB_MAX_READERS         ((SPIKEDB_READER_TABLE_PAGES * SPIKEDB_PAGE_SIZE - 16) / 16)
/*  (8192 - 16) / 16 = 511 reader slots */

typedef struct SpikeDB_ReaderSlot {
    volatile uint64_t txn_id;      /* 0 = slot is free                    */
    volatile uint32_t pid;         /* OS process ID (for stale detection) */
    uint32_t          _pad;
} SpikeDB_ReaderSlot;              /* 16 bytes each */

/* Reader Table Header — first 16 bytes of page 2 */
typedef struct SpikeDB_ReaderTableHeader {
    uint32_t magic;                /* 0x52445442 = "RDTB" */
    uint32_t max_readers;
    uint8_t  _pad[8];
} SpikeDB_ReaderTableHeader;

#define SPIKEDB_READER_TABLE_MAGIC  0x52445442U

/*============================================================================
 * SIMD dispatch table
 *============================================================================*/

typedef struct SpikeDB_SimdOps {
    bool (*bloom_check)(const SpikeDB_BloomBlock* blk, uint64_t hash);
    int  (*find_key)(const uint64_t* hashes, uint16_t count, uint64_t target);
    int  (*find_exit)(const uint64_t* hashes, uint16_t count, uint64_t target);
} SpikeDB_SimdOps;

/*============================================================================
 * Database handle
 *============================================================================*/

typedef struct SpikeDB {
    void*                      mapping;        /* mmap base pointer              */
    uint64_t                   mapping_size;
    void*                      file_handle;    /* HANDLE (Windows)               */
    void*                      map_handle;     /* HANDLE for file mapping        */

    /* Double-buffered meta pages (pointers into the mapping) */
    SpikeDB_MetaPage*          meta[2];        /* meta[0] = page 0, meta[1] = page 1 */
    int                        active_meta;    /* 0 or 1: which has higher txn_id */

    /* Convenience pointer to the currently-active meta state.  Updated after
     * each commit (write) and on open. */
    volatile SpikeDB_MetaPage* cur_meta;

    /* Reader table (pointer into the mapping, pages 2-3) */
    SpikeDB_ReaderTableHeader* reader_hdr;
    SpikeDB_ReaderSlot*        reader_slots;

    /* Cross-process write mutex — only one writer at a time */
    void*                      write_mutex;    /* HANDLE to named mutex          */

    SpikeDB_SimdLevel          simd_level;
    SpikeDB_SimdOps            ops;

    bool                       exclusive;     /* true = no mutex / reader table */
} SpikeDB;

/* Return codes */
typedef enum {
    SPIKEDB_OK         =  0,
    SPIKEDB_NOT_FOUND  = -1,
    SPIKEDB_ERROR      = -2,
    SPIKEDB_FULL       = -3,
} SpikeDB_Status;

/*============================================================================
 * WriteBatch
 *============================================================================*/

#define SPIKEDB_BATCH_OP_PUT    0
#define SPIKEDB_BATCH_OP_DELETE 1

typedef struct SpikeDB_WriteBatch {
    uint8_t* data;
    size_t   size;
    size_t   capacity;
    int      count;
} SpikeDB_WriteBatch;

/*============================================================================
 * API
 *============================================================================*/

SpikeDB_Status spike_db_open(SpikeDB** db, const char* path, uint32_t max_size_gb,
                             uint32_t flags);
void           spike_db_close(SpikeDB* db);

SpikeDB_Status spike_db_get(SpikeDB* db,
                             const char* key, size_t keylen,
                             char** val_out, size_t* vallen_out);

SpikeDB_Status spike_db_put(SpikeDB* db,
                             const char* key, size_t keylen,
                             const char* val, size_t vallen);

SpikeDB_Status spike_db_delete(SpikeDB* db, const char* key, size_t keylen);

void spike_db_free(void* ptr);

/* ---- WriteBatch ---- */

SpikeDB_WriteBatch* spike_db_writebatch_create(void);
void                spike_db_writebatch_destroy(SpikeDB_WriteBatch* batch);
void                spike_db_writebatch_put(SpikeDB_WriteBatch* batch,
                                             const char* key, size_t keylen,
                                             const char* val, size_t vallen);
void                spike_db_writebatch_delete(SpikeDB_WriteBatch* batch,
                                                const char* key, size_t keylen);
void                spike_db_writebatch_clear(SpikeDB_WriteBatch* batch);
int                 spike_db_writebatch_count(const SpikeDB_WriteBatch* batch);
SpikeDB_Status      spike_db_write(SpikeDB* db, SpikeDB_WriteBatch* batch);

/* Query SIMD level */
SpikeDB_SimdLevel spike_db_simd_level(SpikeDB* db);

#ifdef __cplusplus
}
#endif

#endif /* SPIKE_DB_H */
