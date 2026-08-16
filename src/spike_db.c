/*============================================================================
 * spike_db.c  —  SpikeDB v7 implementation
 *
 * See docs/design.md for design.
 *
 * Conventions:
 *   - All public symbols prefixed `spike_db_` / `SpikeDB_`.
 *   - Internal statics use `snake_case`.
 *   - All page access goes through page_pin / page_unpin / page_dirty.
 *   - All on-disk structs use #pragma pack(push, 1).
 *============================================================================*/

#include "spike_db.h"
#include "spike_db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <intrin.h>      /* __cpuid, _mm_crc32_* */
  #include <nmmintrin.h>   /* _mm_crc32_u64 */
  typedef HANDLE spdb_fd_t;
  #define SPDB_INVALID_FD INVALID_HANDLE_VALUE
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <time.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #if defined(__SSE4_2__)
    #include <nmmintrin.h>
  #endif
  typedef int spdb_fd_t;
  #define SPDB_INVALID_FD (-1)
#endif

/*============================================================================
 * Helpers
 *============================================================================*/

#define SPDB_UNUSED(x) ((void)(x))
#define SPDB_MIN(a,b)  ((a)<(b)?(a):(b))
#define SPDB_MAX(a,b)  ((a)>(b)?(a):(b))

/* Structural audit hook (docs/testing.md §5): walks the handle's cache and
 * hash table at points where no operation should be in flight. Compiled out
 * unless SPIKEDB_AUDIT is defined; the test build turns it on. */
#ifdef SPIKEDB_AUDIT
static void spdb_audit(SpikeDB* db, const char* where) {
    char msg[192];
    int n = spike_db_internal_check(db, msg, sizeof(msg));
    if (n) {
        fprintf(stderr, "\nspikedb: %d invariant failure(s) after %s: %s\n",
                n, where, msg);
        fflush(stderr);
        abort();
    }
}
#define SPDB_AUDIT(db) spdb_audit((db), __func__)
#else
#define SPDB_AUDIT(db) ((void)0)
#endif

/* Every page reserves its last 4 bytes for a CRC-32C of the bytes before
 * it. The meta page already had its checksum there; data pages follow the
 * same convention so one helper can stamp and verify all of them. */
#define SPDB_PAGE_CRC_OFF   (SPIKEDB_PAGE_SIZE - 4u)
#define SPDB_PAGE_BODY      SPDB_PAGE_CRC_OFF

/* Compile-time guard for the on-disk layout. Every size below is baked
 * into files on disk, and every capacity has to stop short of the
 * checksum trailer. */
#define SPDB_STATIC_ASSERT(cond, name) typedef char spdb_assert_##name[(cond) ? 1 : -1]

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Software fallback: CRC-32C (Castagnoli) table-driven. Kept separate from
 * crc32_compute so the suite can assert the two paths agree byte for byte;
 * a divergence makes files written on one machine unreadable on another. */
static uint32_t crc32c_sw(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;

    static uint32_t table[256];
    static int      table_built = 0;
    if (!table_built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ (0x82F63B78u & -(int32_t)(c & 1));
            table[i] = c;
        }
        table_built = 1;
    }
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return ~crc;
}

static uint32_t crc32_compute(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;

#if defined(__SSE4_2__) || defined(_MSC_VER)
    uint32_t crc = 0xFFFFFFFFu;
    /* Hardware CRC32 (SSE4.2). MSVC always exposes the intrinsics; we still
     * runtime-feature-check via __cpuid on the first call.
     *
     * Note: _mm_crc32_u64 computes the *Castagnoli* polynomial (CRC-32C),
     * not the IEEE/zlib polynomial. We only need a strong checksum here
     * (meta page integrity), not interop with another CRC. The choice of
     * polynomial is internal to spike_db, but it must be stable: the
     * software fallback below also computes CRC-32C. */
    static int hw_checked = 0;
    static int hw_ok      = 0;
    if (!hw_checked) {
        hw_checked = 1;
#if defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, 1);
        hw_ok = (regs[2] & (1 << 20)) != 0;   /* ECX bit 20 = SSE4.2 */
#else
        unsigned int eax, ebx, ecx, edx;
        if (__builtin_cpu_supports("sse4.2")) hw_ok = 1;
        (void)eax; (void)ebx; (void)ecx; (void)edx;
#endif
    }
    if (hw_ok) {
        size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
        uint64_t c64 = crc;
        for (; i + 8 <= len; i += 8) {
            uint64_t v;
            memcpy(&v, p + i, 8);
#if defined(_MSC_VER)
            c64 = _mm_crc32_u64(c64, v);
#else
            c64 = __builtin_ia32_crc32di(c64, v);
#endif
        }
        crc = (uint32_t)c64;
#endif
        for (; i + 4 <= len; i += 4) {
            uint32_t v;
            memcpy(&v, p + i, 4);
#if defined(_MSC_VER)
            crc = _mm_crc32_u32(crc, v);
#else
            crc = __builtin_ia32_crc32si(crc, v);
#endif
        }
        for (; i < len; i++) {
#if defined(_MSC_VER)
            crc = _mm_crc32_u8(crc, p[i]);
#else
            crc = __builtin_ia32_crc32qi(crc, p[i]);
#endif
        }
        return ~crc;
    }
#endif

    return crc32c_sw(p, len);
}

/* Internal, not part of the public API: lets the suite compare the two CRC
 * paths in one process. Declared in spike_db_internal.h. */
uint32_t spike_db_internal_crc32c(const void* data, size_t len, int force_software) {
    return force_software ? crc32c_sw(data, len) : crc32_compute(data, len);
}

/*============================================================================
 * On-disk structures
 *============================================================================*/

#pragma pack(push, 1)

typedef struct MetaPage {
    uint64_t magic;
    uint64_t txn_id;
    uint64_t total_pages_allocated;     /* bump-allocator watermark */
    uint32_t freelist_head;             /* page id of first free-list page, or INVALID */
    uint32_t symbol_count;
    uint64_t reader_epoch;              /* bumped each commit */
    uint32_t user_meta_len;             /* bytes used in user_meta */
    uint8_t  user_meta[SPIKEDB_META_CAPACITY];  /* [u16 klen][u16 vlen][key][val]... */
    uint8_t  reserved[SPIKEDB_PAGE_SIZE - 8 - 8 - 8 - 4 - 4 - 8 - 4
                      - SPIKEDB_META_CAPACITY - 4];
    uint32_t crc32;                     /* CRC of bytes [0 .. PAGE_SIZE-4) */
} MetaPage;

/* Symbol directory slot. Open-addressed hash. */
typedef struct SymDirSlot {
    uint64_t symbol;
    uint32_t root_page;
    uint32_t _pad;
} SymDirSlot;

/* Free-list page: header + array of u32 page ids */
typedef struct FreelistPage {
    uint32_t next_page;     /* INVALID if last */
    uint32_t count;         /* entries used in `pages[]` */
    uint8_t  _pad[8];
    /* uint32_t pages[ (PAGE_SIZE - 16) / 4 ] follows */
} FreelistPage;

#define FREELIST_CAPACITY  ((SPDB_PAGE_BODY - sizeof(FreelistPage)) / sizeof(uint32_t))

/* Skip-list index node — packed into "node pages" */
typedef struct SkipNode {
    uint64_t first_time;            /* min time of pointed-to leaf */
    uint32_t first_seq;             /* seq of that leaf's first record */
    uint32_t leaf_page;             /* INVALID for unused slot */
    uint8_t  level;                 /* tower height 1..MAX_LEVEL */
    uint8_t  _pad[3];
    uint64_t forward[SPIKEDB_MAX_LEVEL];   /* node_ref values; 0 = nil */
} SkipNode;

#define NODE_PAGE_HDR_SIZE   16u
#define NODE_PAGE_CAPACITY   ((SPDB_PAGE_BODY - NODE_PAGE_HDR_SIZE) / sizeof(SkipNode))

typedef struct NodePageHeader {
    uint32_t used_count;            /* slots [1..used_count] are valid; slot 0 reserved */
    uint32_t next_node_page;        /* chain of node pages with free space (not used in v1) */
    uint8_t  _pad[8];
    /* SkipNode nodes[NODE_PAGE_CAPACITY] follows; we use 1-based indexing so slot 0 is unused */
} NodePageHeader;

/*-- node_ref encoding: hi 40 bits = page_id, lo 24 bits = slot (1-based; 0 = nil) --*/
#define NODE_REF(page,slot)  (((uint64_t)(page) << 24) | (uint64_t)(slot))
#define NODE_REF_PAGE(r)     ((uint32_t)((r) >> 24))
#define NODE_REF_SLOT(r)     ((uint32_t)((r) & 0xFFFFFFu))
#define NODE_REF_NIL         0ULL

/* SymbolRoot page (one per symbol) */
typedef struct SymbolRootPage {
    uint64_t symbol;
    uint64_t min_time;              /* UINT64_MAX when empty */
    uint64_t max_time;              /* 0 when empty */
    uint64_t record_count;
    uint64_t leaf_count;
    uint32_t first_leaf;            /* INVALID when empty */
    uint32_t last_leaf;
    uint8_t  current_max_level;     /* current tallest in-use level */
    uint8_t  _pad[3];
    uint32_t rng_state;
    uint32_t current_node_page;     /* node page currently being packed (INVALID = none) */
    uint32_t record_size;           /* 0 = variable-width, else fixed columnar leaves */
    uint64_t head_forward[SPIKEDB_MAX_LEVEL];   /* sentinel head's forward refs */
    /* pad to PAGE_SIZE */
} SymbolRootPage;

/* Leaf page header.
 *
 * Variable-width leaves: slot dir grows up after the header, value heap
 * grows down from SPDB_PAGE_BODY.
 *
 * Fixed-width leaves (record_size != 0): three columnar arrays of fixed
 * capacity, so a range read is one memcpy out of the payload column and
 * there is no per-record slot to store.
 *   times[cap] | seqs[cap] | payloads[cap * record_size]
 *
 * The header is padded to 64 bytes so those arrays stay naturally aligned.
 */
typedef struct LeafHeader {
    uint64_t symbol;
    uint64_t min_time;
    uint64_t max_time;
    uint32_t record_count;
    uint32_t value_heap_bottom;     /* variable-width only */
    uint32_t prev_leaf;
    uint32_t next_leaf;
    uint64_t skiplist_node_ref;     /* node_ref of this leaf's index entry */
    uint32_t record_size;
    uint8_t  _pad[12];
} LeafHeader;

typedef struct LeafSlot {
    uint64_t time;
    uint32_t seq;                   /* tiebreaker for equal timestamps */
    uint32_t value_offset;          /* byte offset within page */
    uint16_t value_len;
    uint16_t _pad;
} LeafSlot;

#pragma pack(pop)

#define LEAF_SLOT_SIZE       (sizeof(LeafSlot))
#define LEAF_HDR_SIZE        (sizeof(LeafHeader))

/* Fixed-width leaf geometry. */
#define LEAF_FIXED_STRIDE(R)  (8u + 4u + (uint32_t)(R))
#define LEAF_FIXED_CAP(R)     ((uint32_t)((SPDB_PAGE_BODY - (uint32_t)LEAF_HDR_SIZE) \
                                          / LEAF_FIXED_STRIDE(R)))

/* Largest record_size that still fits a useful number of rows per leaf. */
#define LEAF_FIXED_MAX_REC    16384u

SPDB_STATIC_ASSERT(sizeof(MetaPage) == SPIKEDB_PAGE_SIZE,        meta_page_size);
SPDB_STATIC_ASSERT(sizeof(LeafHeader) == 64,                     leaf_header_size);
SPDB_STATIC_ASSERT(sizeof(LeafSlot) == 20,                       leaf_slot_size);
SPDB_STATIC_ASSERT(sizeof(SkipNode) == 148,                      skip_node_size);
SPDB_STATIC_ASSERT(sizeof(SymDirSlot) == 16,                     symdir_slot_size);
/* The fixed layout casts uint64_t/uint32_t at these offsets. */
SPDB_STATIC_ASSERT(sizeof(LeafHeader) % 8 == 0,                  leaf_header_aligned);
SPDB_STATIC_ASSERT(NODE_PAGE_HDR_SIZE + NODE_PAGE_CAPACITY * sizeof(SkipNode)
                   <= SPDB_PAGE_BODY,                            node_page_fits);
SPDB_STATIC_ASSERT(sizeof(FreelistPage) + FREELIST_CAPACITY * sizeof(uint32_t)
                   <= SPDB_PAGE_BODY,                            freelist_fits);
SPDB_STATIC_ASSERT(LEAF_HDR_SIZE + LEAF_FIXED_CAP(1) * LEAF_FIXED_STRIDE(1)
                   <= SPDB_PAGE_BODY,                            fixed_leaf_fits_min);
SPDB_STATIC_ASSERT(LEAF_HDR_SIZE + LEAF_FIXED_CAP(LEAF_FIXED_MAX_REC)
                                 * LEAF_FIXED_STRIDE(LEAF_FIXED_MAX_REC)
                   <= SPDB_PAGE_BODY,                            fixed_leaf_fits_max);

/* Total order on the (time, seq) part of the primary key. */
static int key_cmp(uint64_t t1, uint32_t s1, uint64_t t2, uint32_t s2) {
    if (t1 != t2) return t1 < t2 ? -1 : 1;
    if (s1 != s2) return s1 < s2 ? -1 : 1;
    return 0;
}

/*============================================================================
 * Page cache
 *============================================================================*/

typedef struct CacheSlot {
    uint32_t page_id;       /* INVALID = empty */
    uint32_t pin_count;
    uint64_t last_used;
    uint8_t* bytes;
    bool     dirty;
    bool     valid;
    bool     fresh;         /* true if page_id was allocated by the current
                             * transaction (page_pin_zero set it). Only such
                             * pages are safe to dirty-spill mid-txn, because
                             * they have no valid on-disk predecessor that a
                             * rollback would need to restore. */
} CacheSlot;

struct SpikeDB {
    spdb_fd_t          fd;
    uint64_t           file_size;          /* bytes */
    uint32_t           cache_capacity;
    CacheSlot*         slots;
    uint8_t*           storage;            /* aligned cap * PAGE_SIZE */
    uint32_t*          ht;                 /* open-addressing: page_id -> slot_idx */
    uint32_t           ht_mask;
    uint64_t           clock;
    uint32_t           clock_hand;

    /* Every page read/write, fsync and file lock goes through here. Always
     * &spike_db_internal_io_os except while a test has a wrapper installed;
     * see spike_db_internal.h. */
    const SpikeDB_IoOps* io;

    /* meta */
    MetaPage           meta_buf[2];
    int                active_meta;        /* 0 or 1 */
    bool               readonly;

    /* stats */
    uint64_t           cache_hits;
    uint64_t           cache_misses;
    uint64_t           cache_spills;       /* dirty evictions written early */

    /* current write transaction (single-writer) */
    bool               in_txn;
    uint32_t*          txn_allocated;      /* page ids allocated this txn */
    uint32_t           txn_allocated_count;
    uint32_t           txn_allocated_cap;
    /* On commit, all dirty pages get flushed; meta is then flipped. */
    /* On rollback, allocated pages are returned to the freelist and
       any dirty cache slots are evicted (since their content is the
       half-applied txn). */

    /* Scratch buffer reused by leaf_compact (avoids per-split malloc). */
    uint8_t*           scratch_page;

    /* Set whenever cache_evict fails to find a victim (all slots pinned
     * or dirty). The current transaction can no longer make progress;
     * spike_db_write / spike_db_truncate_before convert this into a
     * SPIKEDB_FULL return after rolling back. Reset on txn_begin. */
    bool               cache_oom;

    SpikeDB_Error      last_error;
};

/*============================================================================
 * Error reporting
 *============================================================================*/

static int os_last_error(void) {
#ifdef _WIN32
    return (int)GetLastError();
#else
    return errno;
#endif
}

static SpikeDB_Status set_err(SpikeDB* db, SpikeDB_Status st, int os_err,
                              uint64_t page, const char* msg) {
    if (db) {
        db->last_error.status   = st;
        db->last_error.os_error = os_err;
        db->last_error.page     = page;
        snprintf(db->last_error.message, sizeof(db->last_error.message), "%s", msg);
    }
    return st;
}

/*============================================================================
 * I/O helpers
 *
 * io_read / io_write / io_fsync / file_lock / file_unlock dispatch through
 * db->io so a test can install a wrapper; the *_os functions below are the
 * real implementations and the installed default.
 *============================================================================*/

static SpikeDB_Status io_read_os(SpikeDB* db, uint32_t page_id, void* buf) {
    uint64_t off = (uint64_t)page_id * SPIKEDB_PAGE_SIZE;
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(db->fd, buf, SPIKEDB_PAGE_SIZE, &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            memset(buf, 0, SPIKEDB_PAGE_SIZE);
            return SPIKEDB_OK;
        }
        return set_err(db, SPIKEDB_ERROR, os_last_error(), page_id, "page read failed");
    }
    if (got < SPIKEDB_PAGE_SIZE)
        memset((uint8_t*)buf + got, 0, SPIKEDB_PAGE_SIZE - got);
#else
    ssize_t n = pread(db->fd, buf, SPIKEDB_PAGE_SIZE, (off_t)off);
    if (n < 0) return set_err(db, SPIKEDB_ERROR, os_last_error(), page_id, "page read failed");
    if ((size_t)n < SPIKEDB_PAGE_SIZE)
        memset((uint8_t*)buf + n, 0, SPIKEDB_PAGE_SIZE - (size_t)n);
#endif
    return SPIKEDB_OK;
}

static SpikeDB_Status io_write_os(SpikeDB* db, uint32_t page_id, const void* buf) {
    uint64_t off = (uint64_t)page_id * SPIKEDB_PAGE_SIZE;
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD wrote = 0;
    if (!WriteFile(db->fd, buf, SPIKEDB_PAGE_SIZE, &wrote, &ov))
        return set_err(db, SPIKEDB_ERROR, os_last_error(), page_id, "page write failed");
    if (wrote != SPIKEDB_PAGE_SIZE)
        return set_err(db, SPIKEDB_ERROR, 0, page_id, "short page write");
#else
    ssize_t n = pwrite(db->fd, buf, SPIKEDB_PAGE_SIZE, (off_t)off);
    if (n != (ssize_t)SPIKEDB_PAGE_SIZE)
        return set_err(db, SPIKEDB_ERROR, os_last_error(), page_id, "page write failed");
#endif
    uint64_t needed = off + SPIKEDB_PAGE_SIZE;
    if (needed > db->file_size) db->file_size = needed;
    return SPIKEDB_OK;
}

static SpikeDB_Status io_fsync_os(SpikeDB* db) {
#ifdef _WIN32
    if (FlushFileBuffers(db->fd)) return SPIKEDB_OK;
#else
    if (fdatasync(db->fd) == 0) return SPIKEDB_OK;
#endif
    return set_err(db, SPIKEDB_ERROR, os_last_error(), SPIKEDB_INVALID_PAGE, "fsync failed");
}

static SpikeDB_Status io_read(SpikeDB* db, uint32_t page_id, void* buf) {
    return db->io->read(db, page_id, buf);
}

static SpikeDB_Status io_write(SpikeDB* db, uint32_t page_id, const void* buf) {
    return db->io->write(db, page_id, buf);
}

static SpikeDB_Status io_fsync(SpikeDB* db) {
    return db->io->fsync(db);
}

/* Forward decl: needed by db_refresh_meta below. */
static void ht_remove(SpikeDB* db, uint32_t page_id);

/*============================================================================
 * File range locks for multi-process safety
 *
 * We reserve a single byte region in the file at SPDB_LOCK_OFFSET (well
 * past any real data — 1 TiB) used for two purposes:
 *
 *   - Exclusive writer lock: held by spike_db_write / truncate_before for
 *     the full transaction. Blocks other writers in other processes.
 *   - Shared reader lock: held by read APIs for the duration of one call.
 *     Blocks during another writer's commit, but otherwise concurrent.
 *============================================================================*/

#define SPDB_LOCK_OFFSET   ((uint64_t)1 << 40)
#define SPDB_LOCK_LEN      1u

static SpikeDB_Status file_lock_os(SpikeDB* db, bool exclusive) {
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(SPDB_LOCK_OFFSET & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(SPDB_LOCK_OFFSET >> 32);
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!LockFileEx(db->fd, flags, 0, SPDB_LOCK_LEN, 0, &ov))
        return set_err(db, SPIKEDB_ERROR, os_last_error(), SPIKEDB_INVALID_PAGE,
                       exclusive ? "exclusive file lock failed" : "shared file lock failed");
    return SPIKEDB_OK;
#else
    struct flock fl;
    fl.l_type   = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = (off_t)SPDB_LOCK_OFFSET;
    fl.l_len    = SPDB_LOCK_LEN;
    fl.l_pid    = 0;
  #ifdef F_OFD_SETLKW
    if (fcntl(db->fd, F_OFD_SETLKW, &fl) == 0) return SPIKEDB_OK;
  #endif
    if (fcntl(db->fd, F_SETLKW, &fl) == 0) return SPIKEDB_OK;
    return set_err(db, SPIKEDB_ERROR, os_last_error(), SPIKEDB_INVALID_PAGE,
                   exclusive ? "exclusive file lock failed" : "shared file lock failed");
#endif
}

static void file_unlock_os(SpikeDB* db) {
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(SPDB_LOCK_OFFSET & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(SPDB_LOCK_OFFSET >> 32);
    UnlockFileEx(db->fd, 0, SPDB_LOCK_LEN, 0, &ov);
#else
    struct flock fl;
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = (off_t)SPDB_LOCK_OFFSET;
    fl.l_len    = SPDB_LOCK_LEN;
    fl.l_pid    = 0;
  #ifdef F_OFD_SETLK
    if (fcntl(db->fd, F_OFD_SETLK, &fl) == 0) return;
  #endif
    fcntl(db->fd, F_SETLK, &fl);
#endif
}

const SpikeDB_IoOps spike_db_internal_io_os = {
    io_read_os, io_write_os, io_fsync_os, file_lock_os, file_unlock_os, NULL
};

const SpikeDB_IoOps* spike_db_internal_set_io(SpikeDB* db, const SpikeDB_IoOps* ops) {
    if (!db) return NULL;
    const SpikeDB_IoOps* prev = db->io;
    db->io = ops ? ops : &spike_db_internal_io_os;
    return prev;
}

void* spike_db_internal_io_ctx(SpikeDB* db) {
    return db ? db->io->ctx : NULL;
}

static SpikeDB_Status file_lock(SpikeDB* db, bool exclusive) {
    return db->io->lock(db, exclusive);
}

static void file_unlock(SpikeDB* db) {
    db->io->unlock(db);
}

static SpikeDB_Status meta_validate(const MetaPage* m) {
    if (m->magic != SPIKEDB_MAGIC) return SPIKEDB_CORRUPT;
    uint32_t want = crc32_compute(m, sizeof(MetaPage) - sizeof(uint32_t));
    if (m->crc32 != want) return SPIKEDB_CORRUPT;
    return SPIKEDB_OK;
}

/* Re-read both meta pages from disk and pick the one with the higher
 * valid txn_id. If active_meta changed (another writer committed),
 * invalidate all clean unpinned cache slots since their content may
 * be stale. Dirty/pinned slots are left alone (only the writer mutates
 * them, and the writer already holds the exclusive lock when calling). */
static SpikeDB_Status db_refresh_meta(SpikeDB* db) {
    MetaPage a, b;
    if (io_read(db, SPIKEDB_META_A_PAGE, &a) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (io_read(db, SPIKEDB_META_B_PAGE, &b) != SPIKEDB_OK) return SPIKEDB_ERROR;

    int new_active;
    uint64_t new_txn;
    bool va = meta_validate(&a) == SPIKEDB_OK;
    bool vb = meta_validate(&b) == SPIKEDB_OK;
    if (!va && !vb) return SPIKEDB_CORRUPT;
    if (va && vb) { new_active = (a.txn_id >= b.txn_id) ? 0 : 1; }
    else { new_active = va ? 0 : 1; }
    new_txn = (new_active == 0) ? a.txn_id : b.txn_id;

    if (new_txn == db->meta_buf[db->active_meta].txn_id
        && new_active == db->active_meta) {
        /* No change */
        return SPIKEDB_OK;
    }
    db->meta_buf[0]  = a;
    db->meta_buf[1]  = b;
    db->active_meta  = new_active;

    /* Invalidate clean, unpinned cache slots — their on-disk content
     * may have been overwritten by another process's commit. */
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        CacheSlot* s = &db->slots[i];
        if (!s->valid) continue;
        if (s->pin_count > 0) continue;
        if (s->dirty) continue;
        ht_remove(db, s->page_id);
        s->valid   = false;
        s->page_id = SPIKEDB_INVALID_PAGE;
    }
    return SPIKEDB_OK;
}

/*============================================================================
 * Page cache
 *============================================================================*/

#define HT_EMPTY  0xFFFFFFFFu

/* Stamp the trailer and write. Every data page goes out through here so
 * there is exactly one place that can forget the checksum. */
static SpikeDB_Status page_write_checked(SpikeDB* db, uint32_t page_id, uint8_t* bytes) {
    uint32_t crc = crc32_compute(bytes, SPDB_PAGE_BODY);
    memcpy(bytes + SPDB_PAGE_CRC_OFF, &crc, 4);
    return io_write(db, page_id, bytes);
}

static SpikeDB_Status page_read_checked(SpikeDB* db, uint32_t page_id, uint8_t* bytes) {
    if (io_read(db, page_id, bytes) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint32_t stored;
    memcpy(&stored, bytes + SPDB_PAGE_CRC_OFF, 4);
    if (stored != crc32_compute(bytes, SPDB_PAGE_BODY))
        return set_err(db, SPIKEDB_CORRUPT, 0, page_id, "page checksum mismatch");
    return SPIKEDB_OK;
}

static uint32_t ht_probe(const SpikeDB* db, uint32_t page_id) {
    uint32_t mask = db->ht_mask;
    uint32_t i = (uint32_t)(mix64(page_id) & mask);
    while (db->ht[i] != HT_EMPTY && db->slots[db->ht[i]].page_id != page_id)
        i = (i + 1) & mask;
    return i;
}

static void ht_insert(SpikeDB* db, uint32_t page_id, uint32_t slot_idx) {
    uint32_t mask = db->ht_mask;
    uint32_t i = (uint32_t)(mix64(page_id) & mask);
    while (db->ht[i] != HT_EMPTY) i = (i + 1) & mask;
    db->ht[i] = slot_idx;
    SPDB_UNUSED(page_id);
}

static void ht_remove(SpikeDB* db, uint32_t page_id) {
    uint32_t mask = db->ht_mask;
    uint32_t i = ht_probe(db, page_id);
    if (db->ht[i] == HT_EMPTY) return;
    db->ht[i] = HT_EMPTY;
    /* Backward-shift to keep probe chains intact. */
    uint32_t j = (i + 1) & mask;
    while (db->ht[j] != HT_EMPTY) {
        uint32_t k_page = db->slots[db->ht[j]].page_id;
        uint32_t k_home = (uint32_t)(mix64(k_page) & mask);
        bool needs_move;
        if (k_home <= j)
            needs_move = (i >= k_home && i < j);
        else
            needs_move = (i >= k_home || i < j);
        if (needs_move) {
            db->ht[i] = db->ht[j];
            db->ht[j] = HT_EMPTY;
            i = j;
        }
        j = (j + 1) & mask;
    }
}

/* Find a victim slot for eviction. Clock-style sweep. */
static uint32_t cache_evict(SpikeDB* db) {
    /* Two passes around the clock:
     *   Pass 1: prefer a clean unpinned victim (no I/O).
     *   Pass 2: if none, spill a dirty unpinned victim to disk and reuse it.
     *
     * Dirty spill is safe because every dirty page during a transaction is
     * a freshly-allocated page id that is not yet reachable from any
     * committed meta page. Writing it to disk early does not publish it;
     * publication only happens at the meta flip in txn_commit. On crash,
     * the previous meta is still active and the spilled bytes are
     * unreachable garbage (reclaimed via freelist on the next clean
     * commit's allocation, or simply leaked beyond the bump pointer). */
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t scan = 0; scan < db->cache_capacity; scan++) {
            uint32_t i = db->clock_hand;
            db->clock_hand = (db->clock_hand + 1) % db->cache_capacity;
            CacheSlot* s = &db->slots[i];
            if (!s->valid) return i;
            if (s->pin_count > 0) continue;
            if (s->dirty) {
                if (pass == 0) continue;        /* try clean first */
                /* Spill is only safe for pages allocated by the current
                 * transaction. Pre-existing pages are modified in place;
                 * writing their mid-txn bytes to disk would corrupt the
                 * pre-txn copy that rollback / a concurrent reader's old
                 * meta still needs. */
                if (!s->fresh) continue;
                if (page_write_checked(db, s->page_id, s->bytes) != SPIKEDB_OK)
                    continue;                   /* I/O error: try next */
                s->dirty = false;
                db->cache_spills++;
            }
            ht_remove(db, s->page_id);
            s->valid   = false;
            s->fresh   = false;
            s->page_id = SPIKEDB_INVALID_PAGE;
            return i;
        }
    }
    return UINT32_MAX;
}

/* Pin a page into cache, returning a pointer to its bytes. */
static uint8_t* page_pin(SpikeDB* db, uint32_t page_id) {
    if (page_id == SPIKEDB_INVALID_PAGE) return NULL;
    uint32_t i = ht_probe(db, page_id);
    if (db->ht[i] != HT_EMPTY) {
        uint32_t s = db->ht[i];
        db->slots[s].pin_count++;
        db->slots[s].last_used = ++db->clock;
        db->cache_hits++;
        return db->slots[s].bytes;
    }
    db->cache_misses++;
    uint32_t s = cache_evict(db);
    if (s == UINT32_MAX) { db->cache_oom = true; return NULL; }
    CacheSlot* slot = &db->slots[s];
    if (page_read_checked(db, page_id, slot->bytes) != SPIKEDB_OK) return NULL;
    slot->page_id   = page_id;
    slot->pin_count = 1;
    slot->dirty     = false;
    slot->valid     = true;
    slot->fresh     = false;
    slot->last_used = ++db->clock;
    ht_insert(db, page_id, s);
    return slot->bytes;
}

/* Pin a page WITHOUT reading from disk (for freshly-allocated pages).
 * Caller is expected to overwrite the buffer entirely. */
static uint8_t* page_pin_zero(SpikeDB* db, uint32_t page_id) {
    if (page_id == SPIKEDB_INVALID_PAGE) return NULL;
    uint32_t i = ht_probe(db, page_id);
    if (db->ht[i] != HT_EMPTY) {
        uint32_t s = db->ht[i];
        db->slots[s].pin_count++;
        db->slots[s].last_used = ++db->clock;
        memset(db->slots[s].bytes, 0, SPIKEDB_PAGE_SIZE);
        db->slots[s].dirty = true;
        db->slots[s].fresh = db->in_txn;
        return db->slots[s].bytes;
    }
    uint32_t s = cache_evict(db);
    if (s == UINT32_MAX) { db->cache_oom = true; return NULL; }
    CacheSlot* slot = &db->slots[s];
    memset(slot->bytes, 0, SPIKEDB_PAGE_SIZE);
    slot->page_id   = page_id;
    slot->pin_count = 1;
    slot->dirty     = true;
    slot->valid     = true;
    slot->fresh     = db->in_txn;
    slot->last_used = ++db->clock;
    ht_insert(db, page_id, s);
    return slot->bytes;
}

static void page_unpin(SpikeDB* db, uint32_t page_id) {
    uint32_t i = ht_probe(db, page_id);
    if (db->ht[i] == HT_EMPTY) return;
    CacheSlot* s = &db->slots[db->ht[i]];
    if (s->pin_count > 0) s->pin_count--;
}

static void page_dirty(SpikeDB* db, uint32_t page_id) {
    uint32_t i = ht_probe(db, page_id);
    if (db->ht[i] == HT_EMPTY) return;
    db->slots[db->ht[i]].dirty = true;
}

/* Flush all dirty pages, sorted by page id so the kernel/storage sees
 * sequential writes and can coalesce them. With a typical 32-128 dirty
 * pages per batch, this is a meaningful win over an iteration-order
 * flush. */
static SpikeDB_Status cache_flush_all(SpikeDB* db) {
    /* Encode (page_id<<32)|slot_idx so a single u64 sort produces both
     * a page-ordered traversal and the slot index to look up bytes. */
    uint64_t  small[256];
    uint64_t* keys = small;
    uint32_t  count = 0;
    uint32_t  cap   = (uint32_t)(sizeof(small) / sizeof(small[0]));
    bool keys_heap = false;
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        CacheSlot* s = &db->slots[i];
        if (!s->valid || !s->dirty) continue;
        if (count == cap) {
            uint32_t newcap = cap * 2;
            uint64_t* nu;
            if (keys_heap) nu = (uint64_t*)realloc(keys, newcap * sizeof(uint64_t));
            else {
                nu = (uint64_t*)malloc(newcap * sizeof(uint64_t));
                if (nu) memcpy(nu, keys, count * sizeof(uint64_t));
            }
            if (!nu) { if (keys_heap) free(keys); return SPIKEDB_ERROR; }
            keys = nu; cap = newcap; keys_heap = true;
        }
        keys[count++] = ((uint64_t)s->page_id << 32) | (uint64_t)i;
    }
    if (count == 0) return SPIKEDB_OK;

    /* Insertion sort for small N is faster than qsort overhead. */
    for (uint32_t i = 1; i < count; i++) {
        uint64_t k = keys[i];
        uint32_t j = i;
        while (j > 0 && keys[j - 1] > k) { keys[j] = keys[j - 1]; j--; }
        keys[j] = k;
    }

    SpikeDB_Status rv = SPIKEDB_OK;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = (uint32_t)(keys[i] & 0xFFFFFFFFu);
        CacheSlot* slot = &db->slots[s];
        if (page_write_checked(db, slot->page_id, slot->bytes) != SPIKEDB_OK) {
            rv = SPIKEDB_ERROR;
            break;
        }
        slot->dirty = false;
        slot->fresh = false;    /* committed: page is permanent now */
    }
    /* Also clear fresh on any slots that were spilled earlier in this
     * txn (already clean, didn't appear in the dirty list above). */
    if (rv == SPIKEDB_OK) {
        for (uint32_t i = 0; i < db->cache_capacity; i++)
            db->slots[i].fresh = false;
    }
    if (keys_heap) free(keys);
    return rv;
}

/* Discard all dirty pages (rollback). Also invalidate any clean-but-fresh
 * slots: those are pages that were spilled mid-txn so their in-cache bytes
 * reflect aborted mid-txn state, and their page ids are about to be
 * returned to the freelist by the rollback. */
static void cache_discard_dirty(SpikeDB* db) {
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        CacheSlot* s = &db->slots[i];
        if (!s->valid) continue;
        if (s->pin_count != 0) { s->fresh = false; continue; }
        if (!s->dirty && !s->fresh) continue;
        ht_remove(db, s->page_id);
        s->valid   = false;
        s->dirty   = false;
        s->fresh   = false;
        s->page_id = SPIKEDB_INVALID_PAGE;
    }
}

/*============================================================================
 * Page allocator (freelist + bump)
 *
 * Allocations during a transaction are tracked in db->txn_allocated so
 * rollback can return them to the freelist. The freelist itself is a
 * linked list of pages, each holding up to FREELIST_CAPACITY page IDs.
 *============================================================================*/

static SpikeDB_Status txn_track_alloc(SpikeDB* db, uint32_t page_id) {
    if (db->txn_allocated_count == db->txn_allocated_cap) {
        uint32_t newcap = db->txn_allocated_cap ? db->txn_allocated_cap * 2 : 64;
        uint32_t* nu = (uint32_t*)realloc(db->txn_allocated, newcap * sizeof(uint32_t));
        if (!nu) return SPIKEDB_ERROR;
        db->txn_allocated     = nu;
        db->txn_allocated_cap = newcap;
    }
    db->txn_allocated[db->txn_allocated_count++] = page_id;
    return SPIKEDB_OK;
}

static SpikeDB_Status page_alloc(SpikeDB* db, uint32_t* out_page_id) {
    MetaPage* m = &db->meta_buf[db->active_meta];
    uint32_t  pid = SPIKEDB_INVALID_PAGE;

    if (m->freelist_head != SPIKEDB_INVALID_PAGE) {
        uint32_t fl = m->freelist_head;
        uint8_t* fp_bytes = page_pin(db, fl);
        if (!fp_bytes) return SPIKEDB_ERROR;
        FreelistPage* fp  = (FreelistPage*)fp_bytes;
        uint32_t*     ids = (uint32_t*)(fp_bytes + sizeof(FreelistPage));
        if (fp->count > 0) {
            fp->count--;
            pid = ids[fp->count];
            page_dirty(db, fl);
            page_unpin(db, fl);
        } else {
            /* Empty page; consume it as the allocation */
            m->freelist_head = fp->next_page;
            page_unpin(db, fl);
            pid = fl;
        }
    }

    if (pid == SPIKEDB_INVALID_PAGE) {
        pid = (uint32_t)m->total_pages_allocated;
        m->total_pages_allocated++;
    }

    if (txn_track_alloc(db, pid) != SPIKEDB_OK) return SPIKEDB_ERROR;
    *out_page_id = pid;
    return SPIKEDB_OK;
}

/* Return a page to the freelist. Used on rollback (allocated pages) and
 * on truncate (freed leaves). */
static SpikeDB_Status page_free(SpikeDB* db, uint32_t page_id) {
    MetaPage* m = &db->meta_buf[db->active_meta];
    uint32_t  fl_head = m->freelist_head;

    if (fl_head != SPIKEDB_INVALID_PAGE) {
        uint8_t* fp_bytes = page_pin(db, fl_head);
        if (!fp_bytes) return SPIKEDB_ERROR;
        FreelistPage* fp = (FreelistPage*)fp_bytes;
        uint32_t* ids = (uint32_t*)(fp_bytes + sizeof(FreelistPage));
        if (fp->count < FREELIST_CAPACITY) {
            ids[fp->count++] = page_id;
            page_dirty(db, fl_head);
            page_unpin(db, fl_head);
            return SPIKEDB_OK;
        }
        page_unpin(db, fl_head);
    }

    /* Need a new freelist head. Use page_id itself to avoid recursion. */
    uint8_t* nb = page_pin_zero(db, page_id);
    if (!nb) return SPIKEDB_ERROR;
    FreelistPage* nf = (FreelistPage*)nb;
    nf->next_page = fl_head;
    nf->count     = 0;
    page_unpin(db, page_id);
    m->freelist_head = page_id;
    return SPIKEDB_OK;
}

/*============================================================================
 * Transaction begin / commit / rollback
 *============================================================================*/

static SpikeDB_Status txn_begin(SpikeDB* db) {
    if (db->in_txn) return SPIKEDB_ERROR;
    if (db->readonly) return SPIKEDB_ERROR;
    db->in_txn = true;
    db->txn_allocated_count = 0;
    db->cache_oom = false;
    /* Stage future writes against the *inactive* meta page so the active
     * one stays consistent until commit. We work directly on the active
     * meta and copy to inactive at commit. */
    return SPIKEDB_OK;
}

static SpikeDB_Status meta_write(SpikeDB* db, int which) {
    MetaPage* m = &db->meta_buf[which];
    m->magic = SPIKEDB_MAGIC;
    m->crc32 = crc32_compute(m, sizeof(MetaPage) - sizeof(uint32_t));
    /* Write meta page directly to disk, bypassing cache (meta pages are
     * never cached as regular pages). */
    return io_write(db, which == 0 ? SPIKEDB_META_A_PAGE : SPIKEDB_META_B_PAGE, m);
}

/* Abandon the in-flight transaction and re-synchronize the handle with what
 * is actually on disk.
 *
 * The writer mutates the *active* meta in place (bump pointer, freelist head,
 * symbol count), so dropping the dirty pages is not enough: those mutations
 * describe pages that were never published. Re-reading both meta pages puts
 * the handle back on whatever the file really says, which is also the right
 * answer when a commit failed *after* the new meta reached the disk. */
static void txn_discard(SpikeDB* db) {
    cache_discard_dirty(db);

    MetaPage a, b;
    if (io_read(db, SPIKEDB_META_A_PAGE, &a) == SPIKEDB_OK
        && io_read(db, SPIKEDB_META_B_PAGE, &b) == SPIKEDB_OK) {
        bool va = meta_validate(&a) == SPIKEDB_OK;
        bool vb = meta_validate(&b) == SPIKEDB_OK;
        if (va || vb) {
            db->meta_buf[0] = a;
            db->meta_buf[1] = b;
            db->active_meta = (va && vb) ? ((a.txn_id >= b.txn_id) ? 0 : 1)
                                         : (va ? 0 : 1);
        }
    }

    db->in_txn = false;
    db->txn_allocated_count = 0;
}

static SpikeDB_Status txn_commit_ex(SpikeDB* db, bool sync) {
    if (!db->in_txn) return SPIKEDB_ERROR;
    /* 1. Flush all dirty data pages */
    if (cache_flush_all(db) != SPIKEDB_OK) goto fail;
    /* 2. fsync to make data durable */
    if (sync && io_fsync(db) != SPIKEDB_OK) goto fail;
    /* 3. Bump txn_id, copy active -> inactive, write inactive meta */
    int new_active = 1 - db->active_meta;
    db->meta_buf[new_active] = db->meta_buf[db->active_meta];
    db->meta_buf[new_active].txn_id++;
    db->meta_buf[new_active].reader_epoch++;
    if (meta_write(db, new_active) != SPIKEDB_OK) goto fail;
    /* 4. fsync meta */
    if (sync && io_fsync(db) != SPIKEDB_OK) goto fail;
    /* 5. Flip in-memory active marker */
    db->active_meta = new_active;
    db->in_txn = false;
    db->txn_allocated_count = 0;
    return SPIKEDB_OK;
fail:
    txn_discard(db);
    return SPIKEDB_ERROR;
}

static SpikeDB_Status txn_commit(SpikeDB* db) {
    return txn_commit_ex(db, true);
}

static void txn_rollback(SpikeDB* db) {
    if (!db->in_txn) return;
    txn_discard(db);
}

/*============================================================================
 * Symbol directory (open-addressing hash, reserved pages)
 *============================================================================*/

#define SYMDIR_SLOTS_PER_PAGE  (SPDB_PAGE_BODY / (uint32_t)sizeof(SymDirSlot))
#define SYMDIR_TOTAL_SLOTS     (SPIKEDB_SYMDIR_PAGES * SYMDIR_SLOTS_PER_PAGE)

SPDB_STATIC_ASSERT(SYMDIR_SLOTS_PER_PAGE * sizeof(SymDirSlot) <= SPDB_PAGE_BODY,
                   symdir_page_fits);

static void symdir_locate(uint64_t symbol, uint32_t* out_page, uint32_t* out_slot) {
    uint64_t h = mix64(symbol);
    uint32_t global = (uint32_t)(h % SYMDIR_TOTAL_SLOTS);
    uint32_t per_pg = SYMDIR_SLOTS_PER_PAGE;
    *out_page = SPIKEDB_SYMDIR_START + (global / per_pg);
    *out_slot = global % per_pg;
}

/* A dropped symbol leaves a tombstone: clearing the slot outright would
 * break the probe chain of any symbol that collided with it. */
#define SYMDIR_TOMBSTONE  SPIKEDB_INVALID_PAGE

static bool symdir_slot_empty(const SymDirSlot* s) {
    return s->root_page == 0 && s->symbol == 0;
}
static bool symdir_slot_live(const SymDirSlot* s) {
    return !symdir_slot_empty(s) && s->root_page != SYMDIR_TOMBSTONE;
}

static SpikeDB_Status symdir_create(SpikeDB* db, uint64_t symbol,
                                    uint32_t pg, uint32_t slot,
                                    uint32_t* out_root) {
    uint32_t root_pg;
    if (page_alloc(db, &root_pg) != SPIKEDB_OK) return SPIKEDB_ERROR;

    uint8_t* rb = page_pin_zero(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    root->symbol            = symbol;
    root->min_time          = UINT64_MAX;
    root->max_time          = 0;
    root->record_count      = 0;
    root->leaf_count        = 0;
    root->first_leaf        = SPIKEDB_INVALID_PAGE;
    root->last_leaf         = SPIKEDB_INVALID_PAGE;
    root->current_max_level = 1;
    root->rng_state         = (uint32_t)mix64(symbol ^ 0xC0FFEEULL);
    root->current_node_page = SPIKEDB_INVALID_PAGE;
    for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++) root->head_forward[i] = NODE_REF_NIL;
    page_unpin(db, root_pg);

    uint8_t* bytes = page_pin(db, pg);
    if (!bytes) return SPIKEDB_ERROR;
    SymDirSlot* s = (SymDirSlot*)bytes + slot;
    s->symbol    = symbol;
    s->root_page = root_pg;
    page_dirty(db, pg);
    page_unpin(db, pg);

    db->meta_buf[db->active_meta].symbol_count++;
    *out_root = root_pg;
    return SPIKEDB_OK;
}

/* Linear-probe to find slot. Returns root_page or INVALID. */
static SpikeDB_Status symdir_lookup(SpikeDB* db, uint64_t symbol,
                                    uint32_t* out_root, bool create_ok) {
    uint32_t pg, slot;
    symdir_locate(symbol, &pg, &slot);
    uint32_t per_pg = SYMDIR_SLOTS_PER_PAGE;
    uint64_t probes = 0;
    uint32_t reuse_pg = SPIKEDB_INVALID_PAGE, reuse_slot = 0;

    while (probes < SYMDIR_TOTAL_SLOTS) {
        uint8_t* bytes = page_pin(db, pg);
        if (!bytes) return SPIKEDB_ERROR;
        SymDirSlot* s = (SymDirSlot*)bytes + slot;

        if (s->root_page == SYMDIR_TOMBSTONE && s->symbol != symbol) {
            if (reuse_pg == SPIKEDB_INVALID_PAGE) { reuse_pg = pg; reuse_slot = slot; }
            page_unpin(db, pg);
        } else if (symdir_slot_empty(s)) {
            if (!create_ok) {
                page_unpin(db, pg);
                return SPIKEDB_NOT_FOUND;
            }
            page_unpin(db, pg);
            /* Prefer an earlier tombstone so the table does not fill up. */
            if (reuse_pg != SPIKEDB_INVALID_PAGE) { pg = reuse_pg; slot = reuse_slot; }
            return symdir_create(db, symbol, pg, slot, out_root);
        } else if (s->symbol == symbol && s->root_page != SYMDIR_TOMBSTONE) {
            uint32_t root = s->root_page;
            page_unpin(db, pg);
            *out_root = root;
            return SPIKEDB_OK;
        } else {
            page_unpin(db, pg);
        }

        /* Linear probe to next slot */
        slot++;
        if (slot == per_pg) { slot = 0; pg++; if (pg >= SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES) pg = SPIKEDB_SYMDIR_START; }
        probes++;
    }
    return SPIKEDB_FULL;
}

/*============================================================================
 * Skip-list node operations
 *============================================================================*/

static SkipNode* node_at(uint8_t* page_bytes, uint32_t slot) {
    /* slot is 1-based */
    return (SkipNode*)(page_bytes + NODE_PAGE_HDR_SIZE) + (slot - 1);
}

/* Allocate a skip-list node, packing into root->current_node_page when
 * possible; allocates a new node page when full. Returns its node_ref. */
static SpikeDB_Status node_alloc_in_root(SpikeDB* db, uint32_t root_pg,
                                        uint64_t* out_ref) {
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    uint32_t cur = root->current_node_page;

    /* Try to use the current node page if it has space. */
    if (cur != SPIKEDB_INVALID_PAGE) {
        uint8_t* nb = page_pin(db, cur);
        if (!nb) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
        NodePageHeader* h = (NodePageHeader*)nb;
        if (h->used_count < NODE_PAGE_CAPACITY) {
            h->used_count++;
            uint32_t slot = h->used_count;        /* 1-based */
            page_dirty(db, cur);
            page_unpin(db, cur);
            page_unpin(db, root_pg);
            *out_ref = NODE_REF(cur, slot);
            return SPIKEDB_OK;
        }
        page_unpin(db, cur);
    }

    /* Allocate a fresh node page. */
    uint32_t pg;
    if (page_alloc(db, &pg) != SPIKEDB_OK) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
    uint8_t* b = page_pin_zero(db, pg);
    if (!b) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
    NodePageHeader* h = (NodePageHeader*)b;
    h->used_count = 1;
    h->next_node_page = root->current_node_page;   /* chain, so drop can free them */
    page_unpin(db, pg);

    root->current_node_page = pg;
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);

    *out_ref = NODE_REF(pg, 1);
    return SPIKEDB_OK;
}

/* Read a node (returns ref to caller-owned scratch); also pins page. */
static SkipNode* node_load(SpikeDB* db, uint64_t ref, uint32_t* out_pinned_page) {
    uint32_t pg = NODE_REF_PAGE(ref);
    uint32_t sl = NODE_REF_SLOT(ref);
    uint8_t* b = page_pin(db, pg);
    if (!b) return NULL;
    *out_pinned_page = pg;
    return node_at(b, sl);
}

/*============================================================================
 * Random level (per-symbol PRNG, p=0.5)
 *============================================================================*/

static int random_level(SymbolRootPage* root) {
    int level = 1;
    while (level < SPIKEDB_MAX_LEVEL) {
        root->rng_state = root->rng_state * 1664525u + 1013904223u;
        if ((root->rng_state >> 16) & 1u) level++;
        else break;
    }
    return level;
}

/*============================================================================
 * Skip-list descent helpers
 *
 * find_le_leaf: find the leaf that *should* contain `time` for this symbol —
 *   i.e. the leaf whose [min_time..max_time] interval covers `time`, OR the
 *   last leaf with first_time <= time.
 *
 * Returns leaf page id and the predecessor node_ref array (per level) for
 * splice operations.
 *============================================================================*/

typedef struct DescentResult {
    uint64_t pred_ref[SPIKEDB_MAX_LEVEL];  /* node_ref of predecessor at each level (NIL = head) */
    uint32_t leaf_page;                     /* INVALID if no leaf <= time */
} DescentResult;

/* `strict` makes the walk stop at the first node whose key is >= the
 * target rather than > it, so pred_ref[] holds the predecessors of the
 * node with exactly that key — what skiplist_unlink needs. */
static SpikeDB_Status descend_ex(SpikeDB* db, uint32_t root_pg,
                                 uint64_t time, uint32_t seq, bool strict,
                                 DescentResult* out) {
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;

    for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++) out->pred_ref[i] = NODE_REF_NIL;
    out->leaf_page = SPIKEDB_INVALID_PAGE;

    uint64_t curr_ref = NODE_REF_NIL;       /* "head" sentinel */
    int level = root->current_max_level - 1;

    /* Walk down from top level */
    for (; level >= 0; level--) {
        for (;;) {
            uint64_t next_ref = (curr_ref == NODE_REF_NIL)
                              ? root->head_forward[level]
                              : 0;
            if (curr_ref != NODE_REF_NIL) {
                /* Re-load curr to read its forward[] */
                uint32_t cp;
                SkipNode* cn = node_load(db, curr_ref, &cp);
                if (!cn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
                next_ref = cn->forward[level];
                page_unpin(db, cp);
            }
            if (next_ref == NODE_REF_NIL) break;
            uint32_t np;
            SkipNode* nn = node_load(db, next_ref, &np);
            if (!nn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
            uint64_t ft = nn->first_time;
            uint32_t fs = nn->first_seq;
            page_unpin(db, np);
            if (key_cmp(ft, fs, time, seq) > (strict ? -1 : 0)) break;
            curr_ref = next_ref;
        }
        out->pred_ref[level] = curr_ref;
    }
    /* curr_ref now holds the rightmost node with first_time <= time, or NIL */
    if (curr_ref != NODE_REF_NIL) {
        uint32_t cp;
        SkipNode* cn = node_load(db, curr_ref, &cp);
        if (!cn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
        out->leaf_page = cn->leaf_page;
        page_unpin(db, cp);
    } else {
        /* No node ≤ time: use first_leaf if any (handles time < min_time) */
        out->leaf_page = root->first_leaf;
    }
    page_unpin(db, root_pg);
    return SPIKEDB_OK;
}

static SpikeDB_Status descend(SpikeDB* db, uint32_t root_pg,
                              uint64_t time, uint32_t seq,
                              DescentResult* out) {
    return descend_ex(db, root_pg, time, seq, false, out);
}

/*============================================================================
 * Leaf operations
 *
 * Everything below works on either layout. Read paths go through
 * leaf_find / leaf_key_at / leaf_val_at so the rest of the engine does not
 * need to know which one a leaf uses.
 *============================================================================*/

static LeafSlot* leaf_slots(uint8_t* page) {
    return (LeafSlot*)(page + LEAF_HDR_SIZE);
}

static uint64_t* leaf_ftimes(uint8_t* page) {
    return (uint64_t*)(page + LEAF_HDR_SIZE);
}
static uint32_t* leaf_fseqs(uint8_t* page) {
    uint32_t cap = LEAF_FIXED_CAP(((LeafHeader*)page)->record_size);
    return (uint32_t*)(page + LEAF_HDR_SIZE + (size_t)8u * cap);
}
static uint8_t* leaf_fvals(uint8_t* page) {
    uint32_t cap = LEAF_FIXED_CAP(((LeafHeader*)page)->record_size);
    return page + LEAF_HDR_SIZE + (size_t)12u * cap;
}

static uint32_t leaf_free_space(const LeafHeader* h) {
    uint32_t slots_end = LEAF_HDR_SIZE + h->record_count * (uint32_t)LEAF_SLOT_SIZE;
    if (slots_end > h->value_heap_bottom) return 0;
    return h->value_heap_bottom - slots_end;
}

/* Index of the first record whose key >= (time, seq), or record_count. */
static uint32_t leaf_find(const uint8_t* page, uint64_t time, uint32_t seq,
                          bool* exact) {
    const LeafHeader* h = (const LeafHeader*)page;
    uint32_t lo = 0, hi = h->record_count;
    *exact = false;

    if (h->record_size) {
        const uint64_t* ts = (const uint64_t*)(page + LEAF_HDR_SIZE);
        uint32_t cap = LEAF_FIXED_CAP(h->record_size);
        const uint32_t* sq = (const uint32_t*)(page + LEAF_HDR_SIZE + (size_t)8u * cap);
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            if (key_cmp(ts[mid], sq[mid], time, seq) < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo < h->record_count && ts[lo] == time && sq[lo] == seq) *exact = true;
        return lo;
    }

    const LeafSlot* slots = (const LeafSlot*)(page + LEAF_HDR_SIZE);
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (key_cmp(slots[mid].time, slots[mid].seq, time, seq) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < h->record_count && slots[lo].time == time && slots[lo].seq == seq)
        *exact = true;
    return lo;
}

static void leaf_key_at(const uint8_t* page, uint32_t i,
                        uint64_t* time_out, uint32_t* seq_out) {
    const LeafHeader* h = (const LeafHeader*)page;
    if (h->record_size) {
        uint32_t cap = LEAF_FIXED_CAP(h->record_size);
        *time_out = ((const uint64_t*)(page + LEAF_HDR_SIZE))[i];
        *seq_out  = ((const uint32_t*)(page + LEAF_HDR_SIZE + (size_t)8u * cap))[i];
    } else {
        const LeafSlot* s = &((const LeafSlot*)(page + LEAF_HDR_SIZE))[i];
        *time_out = s->time;
        *seq_out  = s->seq;
    }
}

static const uint8_t* leaf_val_at(const uint8_t* page, uint32_t i, uint32_t* len_out) {
    const LeafHeader* h = (const LeafHeader*)page;
    if (h->record_size) {
        uint32_t cap = LEAF_FIXED_CAP(h->record_size);
        *len_out = h->record_size;
        return page + LEAF_HDR_SIZE + (size_t)12u * cap + (size_t)i * h->record_size;
    }
    const LeafSlot* s = &((const LeafSlot*)(page + LEAF_HDR_SIZE))[i];
    *len_out = s->value_len;
    return page + s->value_offset;
}

static void leaf_init(uint8_t* page, uint64_t symbol, uint32_t record_size) {
    LeafHeader* h = (LeafHeader*)page;
    memset(page, 0, SPIKEDB_PAGE_SIZE);
    h->symbol            = symbol;
    h->min_time          = UINT64_MAX;
    h->max_time          = 0;
    h->record_count      = 0;
    h->record_size       = record_size;
    h->value_heap_bottom = SPDB_PAGE_BODY;
    h->prev_leaf         = SPIKEDB_INVALID_PAGE;
    h->next_leaf         = SPIKEDB_INVALID_PAGE;
    h->skiplist_node_ref = NODE_REF_NIL;
}

/* Try to insert (time, seq, value) into an existing leaf. With `overwrite`,
 * an existing key is replaced instead of rejected and *replaced is set.
 * Returns:
 *   SPIKEDB_OK    — inserted or replaced
 *   SPIKEDB_FULL  — caller must split
 *   SPIKEDB_INVAL — duplicate and overwrite not requested, or a value whose
 *                   length does not match the symbol's declared record size
 */
static SpikeDB_Status leaf_upsert(uint8_t* page, uint64_t time, uint32_t seq,
                                  const void* value, size_t vlen,
                                  bool overwrite, bool* replaced) {
    LeafHeader* h = (LeafHeader*)page;
    *replaced = false;

    bool exact;
    uint32_t pos = leaf_find(page, time, seq, &exact);

    if (h->record_size) {
        uint32_t R = h->record_size;
        if (vlen != R) return SPIKEDB_INVAL;
        uint64_t* ts = leaf_ftimes(page);
        uint32_t* sq = leaf_fseqs(page);
        uint8_t*  vs = leaf_fvals(page);

        if (exact) {
            if (!overwrite) return SPIKEDB_INVAL;
            memcpy(vs + (size_t)pos * R, value, R);
            *replaced = true;
            return SPIKEDB_OK;
        }
        if (h->record_count >= LEAF_FIXED_CAP(R)) return SPIKEDB_FULL;

        uint32_t tail = h->record_count - pos;
        if (tail) {
            memmove(&ts[pos + 1], &ts[pos], (size_t)tail * 8);
            memmove(&sq[pos + 1], &sq[pos], (size_t)tail * 4);
            memmove(vs + (size_t)(pos + 1) * R, vs + (size_t)pos * R,
                    (size_t)tail * R);
        }
        ts[pos] = time;
        sq[pos] = seq;
        memcpy(vs + (size_t)pos * R, value, R);
        h->record_count++;
        if (time < h->min_time) h->min_time = time;
        if (time > h->max_time) h->max_time = time;
        return SPIKEDB_OK;
    }

    LeafSlot* slots = leaf_slots(page);

    if (exact) {
        if (!overwrite) return SPIKEDB_INVAL;
        LeafSlot* s = &slots[pos];
        if (vlen <= s->value_len) {
            /* Shrinking leaves a hole; the next leaf_compact reclaims it. */
            memcpy(page + s->value_offset, value, vlen);
        } else {
            if (leaf_free_space(h) < vlen) return SPIKEDB_FULL;
            h->value_heap_bottom -= (uint32_t)vlen;
            memcpy(page + h->value_heap_bottom, value, vlen);
            s->value_offset = h->value_heap_bottom;
        }
        s->value_len = (uint16_t)vlen;
        *replaced = true;
        return SPIKEDB_OK;
    }

    /* Need room for one slot + vlen bytes */
    uint32_t needed = (uint32_t)(LEAF_SLOT_SIZE + vlen);
    if (leaf_free_space(h) < needed) return SPIKEDB_FULL;

    /* Place value */
    h->value_heap_bottom -= (uint32_t)vlen;
    memcpy(page + h->value_heap_bottom, value, vlen);

    /* Shift slots[pos..count-1] to [pos+1..count] */
    if (pos < h->record_count) {
        memmove(&slots[pos + 1], &slots[pos],
                (h->record_count - pos) * sizeof(LeafSlot));
    }
    slots[pos].time         = time;
    slots[pos].seq          = seq;
    slots[pos].value_offset = h->value_heap_bottom;
    slots[pos].value_len    = (uint16_t)vlen;
    slots[pos]._pad         = 0;

    h->record_count++;
    if (time < h->min_time) h->min_time = time;
    if (time > h->max_time) h->max_time = time;

    return SPIKEDB_OK;
}

static SpikeDB_Status leaf_insert(uint8_t* page, uint64_t time, uint32_t seq,
                                  const void* value, size_t vlen) {
    bool replaced;
    return leaf_upsert(page, time, seq, value, vlen, false, &replaced);
}

/* Drop the first `n` records of a leaf, shifting the rest down. */
static void leaf_drop_front(uint8_t* page, uint32_t n) {
    LeafHeader* h = (LeafHeader*)page;
    uint32_t tail = h->record_count - n;
    if (h->record_size) {
        uint32_t R = h->record_size;
        uint64_t* ts = leaf_ftimes(page);
        uint32_t* sq = leaf_fseqs(page);
        uint8_t*  vs = leaf_fvals(page);
        memmove(ts, &ts[n], (size_t)tail * 8);
        memmove(sq, &sq[n], (size_t)tail * 4);
        memmove(vs, vs + (size_t)n * R, (size_t)tail * R);
    } else {
        LeafSlot* ls = leaf_slots(page);
        memmove(&ls[0], &ls[n], (size_t)tail * sizeof(LeafSlot));
    }
    h->record_count = tail;
}

/* Remove one record. */
static void leaf_erase(uint8_t* page, uint32_t pos) {
    LeafHeader* h = (LeafHeader*)page;
    uint32_t tail = h->record_count - pos - 1;
    if (h->record_size) {
        uint32_t R = h->record_size;
        uint64_t* ts = leaf_ftimes(page);
        uint32_t* sq = leaf_fseqs(page);
        uint8_t*  vs = leaf_fvals(page);
        memmove(&ts[pos], &ts[pos + 1], (size_t)tail * 8);
        memmove(&sq[pos], &sq[pos + 1], (size_t)tail * 4);
        memmove(vs + (size_t)pos * R, vs + (size_t)(pos + 1) * R, (size_t)tail * R);
    } else {
        LeafSlot* ls = leaf_slots(page);
        memmove(&ls[pos], &ls[pos + 1], (size_t)tail * sizeof(LeafSlot));
    }
    h->record_count--;
}

/* Refresh min_time/max_time from the records actually present. */
static void leaf_refresh_bounds(uint8_t* page) {
    LeafHeader* h = (LeafHeader*)page;
    if (h->record_count == 0) {
        h->min_time = UINT64_MAX;
        h->max_time = 0;
        h->value_heap_bottom = SPDB_PAGE_BODY;
        return;
    }
    uint64_t t; uint32_t s;
    leaf_key_at(page, 0, &t, &s);
    h->min_time = t;
    leaf_key_at(page, h->record_count - 1, &t, &s);
    h->max_time = t;
}

/* Compact a variable-width leaf's value heap. No-op for fixed-width leaves,
 * which cannot fragment. Uses db->scratch_page to avoid a per-call malloc. */
static SpikeDB_Status leaf_compact(SpikeDB* db, uint8_t* page) {
    LeafHeader* h = (LeafHeader*)page;
    if (h->record_size) return SPIKEDB_OK;
    LeafSlot*   slots = leaf_slots(page);
    if (!db->scratch_page) {
#ifdef _WIN32
        db->scratch_page = (uint8_t*)_aligned_malloc(SPIKEDB_PAGE_SIZE, 64);
#else
        if (posix_memalign((void**)&db->scratch_page, 64, SPIKEDB_PAGE_SIZE) != 0)
            db->scratch_page = NULL;
#endif
        if (!db->scratch_page) return SPIKEDB_ERROR;
    }
    uint8_t* tmp = db->scratch_page;
    uint32_t cursor = SPDB_PAGE_BODY;
    for (uint32_t i = 0; i < h->record_count; i++) {
        uint16_t vl = slots[i].value_len;
        cursor -= vl;
        memcpy(tmp + cursor, page + slots[i].value_offset, vl);
        slots[i].value_offset = cursor;
    }
    memcpy(page + cursor, tmp + cursor, SPDB_PAGE_BODY - cursor);
    h->value_heap_bottom = cursor;
    return SPIKEDB_OK;
}

/*============================================================================
 * Skip-list splice (insert new index node)
 *============================================================================*/

/* Splice a new node with given (level, first_time, leaf_page) using the
 * predecessor refs from a prior descend(). */
static SpikeDB_Status skiplist_splice(SpikeDB* db, uint32_t root_pg,
                                      const DescentResult* desc, int level,
                                      uint64_t first_time, uint32_t first_seq,
                                      uint32_t leaf_pg,
                                      uint64_t* out_new_ref) {
    uint64_t new_ref;
    if (node_alloc_in_root(db, root_pg, &new_ref) != SPIKEDB_OK) return SPIKEDB_ERROR;

    /* Initialize new node */
    {
        uint32_t np;
        SkipNode* nn = node_load(db, new_ref, &np);
        if (!nn) return SPIKEDB_ERROR;
        nn->first_time = first_time;
        nn->first_seq  = first_seq;
        nn->leaf_page  = leaf_pg;
        nn->level      = (uint8_t)level;
        for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++) nn->forward[i] = NODE_REF_NIL;
        page_dirty(db, np);
        page_unpin(db, np);
    }

    /* Splice in: for each level in [0..level-1], set
     *   new_node.forward[i] = pred[i].forward[i]  (or root.head_forward[i])
     *   pred[i].forward[i] (or root.head_forward[i]) = new_ref
     */
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;

    for (int i = 0; i < level; i++) {
        uint64_t pred = desc->pred_ref[i];
        uint64_t old_next;
        if (pred == NODE_REF_NIL) {
            old_next = root->head_forward[i];
            root->head_forward[i] = new_ref;
        } else {
            uint32_t pp;
            SkipNode* pn = node_load(db, pred, &pp);
            if (!pn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
            old_next = pn->forward[i];
            pn->forward[i] = new_ref;
            page_dirty(db, pp);
            page_unpin(db, pp);
        }
        /* Set new_node.forward[i] = old_next */
        uint32_t np;
        SkipNode* nn = node_load(db, new_ref, &np);
        if (!nn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
        nn->forward[i] = old_next;
        page_dirty(db, np);
        page_unpin(db, np);
    }

    if ((uint8_t)level > root->current_max_level)
        root->current_max_level = (uint8_t)level;
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);

    /* Set the leaf's back-pointer to this node */
    {
        uint8_t* lb = page_pin(db, leaf_pg);
        if (!lb) return SPIKEDB_ERROR;
        LeafHeader* lh = (LeafHeader*)lb;
        lh->skiplist_node_ref = new_ref;
        page_dirty(db, leaf_pg);
        page_unpin(db, leaf_pg);
    }

    *out_new_ref = new_ref;
    return SPIKEDB_OK;
}

/*============================================================================
 * Insert single record into a symbol's skip list
 *============================================================================*/

static SpikeDB_Status symbol_insert(SpikeDB* db, uint32_t root_pg,
                                    uint64_t time, uint32_t seq,
                                    const void* value, size_t vlen,
                                    bool overwrite, bool* replaced);

/* Split a leaf into two equal halves around the median, return new leaf id. */
static SpikeDB_Status leaf_split(SpikeDB* db, uint32_t root_pg, uint32_t leaf_pg,
                                 uint32_t* out_new_leaf_pg) {
    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) return SPIKEDB_ERROR;
    LeafHeader* lh = (LeafHeader*)lb;

    uint32_t mid = lh->record_count / 2;

    /* Allocate new leaf */
    uint32_t new_pg;
    if (page_alloc(db, &new_pg) != SPIKEDB_OK) { page_unpin(db, leaf_pg); return SPIKEDB_ERROR; }
    uint8_t* nb = page_pin_zero(db, new_pg);
    if (!nb) { page_unpin(db, leaf_pg); return SPIKEDB_ERROR; }
    leaf_init(nb, lh->symbol, lh->record_size);
    LeafHeader* nh = (LeafHeader*)nb;

    /* Move records [mid..count) to the new leaf. */
    uint32_t moved = lh->record_count - mid;
    if (lh->record_size) {
        uint32_t R = lh->record_size;
        memcpy(leaf_ftimes(nb), &leaf_ftimes(lb)[mid], (size_t)moved * 8);
        memcpy(leaf_fseqs(nb),  &leaf_fseqs(lb)[mid],  (size_t)moved * 4);
        memcpy(leaf_fvals(nb), leaf_fvals(lb) + (size_t)mid * R, (size_t)moved * R);
    } else {
        LeafSlot* ls = leaf_slots(lb);
        LeafSlot* ns = leaf_slots(nb);
        for (uint32_t i = mid; i < lh->record_count; i++) {
            uint16_t vl = ls[i].value_len;
            nh->value_heap_bottom -= vl;
            memcpy(nb + nh->value_heap_bottom, lb + ls[i].value_offset, vl);
            ns[i - mid].time         = ls[i].time;
            ns[i - mid].seq          = ls[i].seq;
            ns[i - mid].value_offset = nh->value_heap_bottom;
            ns[i - mid].value_len    = vl;
            ns[i - mid]._pad         = 0;
        }
    }
    nh->record_count = moved;
    leaf_refresh_bounds(nb);

    /* Truncate old leaf */
    lh->record_count = mid;
    leaf_refresh_bounds(lb);
    if (leaf_compact(db, lb) != SPIKEDB_OK) {
        page_unpin(db, new_pg); page_unpin(db, leaf_pg); return SPIKEDB_ERROR;
    }

    /* Linked list splice: new_leaf goes after old_leaf */
    nh->prev_leaf = leaf_pg;
    nh->next_leaf = lh->next_leaf;
    if (lh->next_leaf != SPIKEDB_INVALID_PAGE) {
        uint8_t* nx = page_pin(db, lh->next_leaf);
        if (nx) {
            ((LeafHeader*)nx)->prev_leaf = new_pg;
            page_dirty(db, lh->next_leaf);
            page_unpin(db, lh->next_leaf);
        }
    }
    lh->next_leaf = new_pg;

    /* Update symbol root: increment leaf_count, possibly update last_leaf */
    {
        uint8_t* rb = page_pin(db, root_pg);
        if (rb) {
            SymbolRootPage* root = (SymbolRootPage*)rb;
            root->leaf_count++;
            if (root->last_leaf == leaf_pg) root->last_leaf = new_pg;
            page_dirty(db, root_pg);
            page_unpin(db, root_pg);
        }
    }

    page_dirty(db, new_pg);
    page_dirty(db, leaf_pg);
    page_unpin(db, new_pg);
    page_unpin(db, leaf_pg);

    /* Add new index node for the new leaf at a random level */
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    int new_level = random_level(root);
    page_unpin(db, root_pg);

    /* Descend with the new leaf's first key */
    DescentResult d;
    uint64_t new_first;
    uint32_t new_first_seq;
    {
        uint8_t* nb2 = page_pin(db, new_pg);
        if (!nb2) return SPIKEDB_ERROR;
        leaf_key_at(nb2, 0, &new_first, &new_first_seq);
        page_unpin(db, new_pg);
    }
    if (descend(db, root_pg, new_first, new_first_seq, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint64_t new_ref;
    if (skiplist_splice(db, root_pg, &d, new_level, new_first, new_first_seq,
                        new_pg, &new_ref) != SPIKEDB_OK)
        return SPIKEDB_ERROR;

    *out_new_leaf_pg = new_pg;
    return SPIKEDB_OK;
}

static SpikeDB_Status symbol_insert(SpikeDB* db, uint32_t root_pg,
                                    uint64_t time, uint32_t seq,
                                    const void* value, size_t vlen,
                                    bool overwrite, bool* replaced) {
    *replaced = false;

    /* Empty symbol? */
    {
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) return SPIKEDB_ERROR;
        SymbolRootPage* root = (SymbolRootPage*)rb;
        if (root->first_leaf == SPIKEDB_INVALID_PAGE) {
            /* Create the very first leaf */
            uint32_t leaf_pg;
            if (page_alloc(db, &leaf_pg) != SPIKEDB_OK) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
            uint8_t* lb = page_pin_zero(db, leaf_pg);
            if (!lb) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
            leaf_init(lb, root->symbol, root->record_size);
            SpikeDB_Status st = leaf_insert(lb, time, seq, value, vlen);
            page_dirty(db, leaf_pg);
            page_unpin(db, leaf_pg);
            if (st != SPIKEDB_OK) { page_unpin(db, root_pg); return st; }

            root->first_leaf   = leaf_pg;
            root->last_leaf    = leaf_pg;
            root->leaf_count   = 1;
            root->record_count = 1;
            root->min_time     = time;
            root->max_time     = time;
            page_dirty(db, root_pg);
            page_unpin(db, root_pg);

            /* Splice into skip list at level 1 (always include lvl 0) */
            int new_level;
            {
                uint8_t* rb2 = page_pin(db, root_pg);
                SymbolRootPage* r2 = (SymbolRootPage*)rb2;
                new_level = random_level(r2);
                page_dirty(db, root_pg);
                page_unpin(db, root_pg);
            }
            DescentResult d;
            for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++) d.pred_ref[i] = NODE_REF_NIL;
            d.leaf_page = SPIKEDB_INVALID_PAGE;
            uint64_t new_ref;
            return skiplist_splice(db, root_pg, &d, new_level, time, seq, leaf_pg, &new_ref);
        }
        page_unpin(db, root_pg);
    }

    /* Append fast path. Ingest is overwhelmingly sorted, so try the tail
     * leaf before paying for a descent. */
    {
        uint32_t tail;
        {
            uint8_t* rb = page_pin(db, root_pg);
            if (!rb) return SPIKEDB_ERROR;
            tail = ((SymbolRootPage*)rb)->last_leaf;
            page_unpin(db, root_pg);
        }
        if (tail != SPIKEDB_INVALID_PAGE) {
            uint8_t* tb = page_pin(db, tail);
            if (!tb) return SPIKEDB_ERROR;
            LeafHeader* th = (LeafHeader*)tb;
            bool placed = false;
            if (th->record_count > 0 && th->next_leaf == SPIKEDB_INVALID_PAGE) {
                uint64_t lt; uint32_t lsq;
                leaf_key_at(tb, th->record_count - 1, &lt, &lsq);
                if (key_cmp(time, seq, lt, lsq) > 0
                    && leaf_upsert(tb, time, seq, value, vlen,
                                   overwrite, replaced) == SPIKEDB_OK) {
                    page_dirty(db, tail);
                    placed = true;
                }
            }
            page_unpin(db, tail);
            if (placed) {
                uint8_t* rb = page_pin(db, root_pg);
                if (!rb) return SPIKEDB_ERROR;
                SymbolRootPage* root = (SymbolRootPage*)rb;
                root->record_count++;
                if (time < root->min_time) root->min_time = time;
                if (time > root->max_time) root->max_time = time;
                page_dirty(db, root_pg);
                page_unpin(db, root_pg);
                return SPIKEDB_OK;
            }
        }
    }

    /* General case: descend, find target leaf */
    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint32_t leaf_pg = d.leaf_page;
    if (leaf_pg == SPIKEDB_INVALID_PAGE) {
        /* key precedes the first leaf's first key — use first_leaf */
        uint8_t* rb = page_pin(db, root_pg);
        leaf_pg = ((SymbolRootPage*)rb)->first_leaf;
        page_unpin(db, root_pg);
    }

    /* Try insert */
    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) return SPIKEDB_ERROR;
    LeafHeader* lh = (LeafHeader*)lb;

    bool after_last = false;
    bool has_room;
    if (lh->record_count > 0) {
        uint64_t lt; uint32_t lsq;
        leaf_key_at(lb, lh->record_count - 1, &lt, &lsq);
        after_last = key_cmp(time, seq, lt, lsq) > 0;
    }
    has_room = lh->record_size
             ? (lh->record_count < LEAF_FIXED_CAP(lh->record_size))
             : (leaf_free_space(lh) >= LEAF_SLOT_SIZE + vlen);

    /* Fast append path: if the key sorts after everything in the last leaf
     * and that leaf is full, start a new leaf instead of splitting. */
    if (after_last && !has_room && lh->next_leaf == SPIKEDB_INVALID_PAGE) {
        uint32_t rec_size = lh->record_size;
        uint64_t sym      = lh->symbol;
        page_unpin(db, leaf_pg);

        uint32_t new_pg;
        if (page_alloc(db, &new_pg) != SPIKEDB_OK) return SPIKEDB_ERROR;
        uint8_t* nb = page_pin_zero(db, new_pg);
        if (!nb) return SPIKEDB_ERROR;
        leaf_init(nb, sym, rec_size);
        SpikeDB_Status st = leaf_insert(nb, time, seq, value, vlen);
        ((LeafHeader*)nb)->prev_leaf = leaf_pg;
        page_dirty(db, new_pg);
        page_unpin(db, new_pg);
        if (st != SPIKEDB_OK) return st;

        /* Link old leaf -> new */
        uint8_t* lb2 = page_pin(db, leaf_pg);
        ((LeafHeader*)lb2)->next_leaf = new_pg;
        page_dirty(db, leaf_pg);
        page_unpin(db, leaf_pg);

        /* Update root */
        uint8_t* rb = page_pin(db, root_pg);
        SymbolRootPage* root = (SymbolRootPage*)rb;
        root->leaf_count++;
        root->record_count++;
        root->last_leaf = new_pg;
        if (time > root->max_time) root->max_time = time;
        if (time < root->min_time) root->min_time = time;
        int new_level = random_level(root);
        page_dirty(db, root_pg);
        page_unpin(db, root_pg);

        /* Splice index node */
        DescentResult d2;
        if (descend(db, root_pg, time, seq, &d2) != SPIKEDB_OK) return SPIKEDB_ERROR;
        uint64_t new_ref;
        return skiplist_splice(db, root_pg, &d2, new_level, time, seq, new_pg, &new_ref);
    }

    SpikeDB_Status st = leaf_upsert(lb, time, seq, value, vlen, overwrite, replaced);
    if (st == SPIKEDB_OK) {
        page_dirty(db, leaf_pg);
        page_unpin(db, leaf_pg);
        if (*replaced) return SPIKEDB_OK;
        /* Update root */
        uint8_t* rb = page_pin(db, root_pg);
        SymbolRootPage* root = (SymbolRootPage*)rb;
        root->record_count++;
        if (time < root->min_time) root->min_time = time;
        if (time > root->max_time) root->max_time = time;
        page_dirty(db, root_pg);
        page_unpin(db, root_pg);
        return SPIKEDB_OK;
    }
    if (st != SPIKEDB_FULL) {
        page_unpin(db, leaf_pg);
        return st;
    }
    /* Full → split */
    page_unpin(db, leaf_pg);

    uint32_t new_pg;
    if (leaf_split(db, root_pg, leaf_pg, &new_pg) != SPIKEDB_OK) return SPIKEDB_ERROR;

    /* Now retry insert (decide which half) */
    uint8_t* lb2 = page_pin(db, leaf_pg);
    LeafHeader* lh2 = (LeafHeader*)lb2;
    uint32_t target = leaf_pg;
    if (lh2->record_count > 0) {
        uint64_t lt; uint32_t lsq;
        leaf_key_at(lb2, lh2->record_count - 1, &lt, &lsq);
        if (key_cmp(time, seq, lt, lsq) > 0) target = new_pg;
    }
    page_unpin(db, leaf_pg);

    uint8_t* tb = page_pin(db, target);
    if (!tb) return SPIKEDB_ERROR;
    SpikeDB_Status st2 = leaf_upsert(tb, time, seq, value, vlen, overwrite, replaced);
    page_dirty(db, target);
    page_unpin(db, target);
    if (st2 != SPIKEDB_OK) return st2;
    if (*replaced) return SPIKEDB_OK;

    /* Update root */
    uint8_t* rb = page_pin(db, root_pg);
    SymbolRootPage* root = (SymbolRootPage*)rb;
    root->record_count++;
    if (time < root->min_time) root->min_time = time;
    if (time > root->max_time) root->max_time = time;
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);
    return SPIKEDB_OK;
}

/*============================================================================
 * Delete
 *============================================================================*/

/* Unlink a node from every level of the skip list. Node slots are never
 * recycled, so the slot itself is left behind. */
static SpikeDB_Status skiplist_unlink(SpikeDB* db, uint32_t root_pg,
                                      uint64_t node_ref,
                                      uint64_t first_time, uint32_t first_seq) {
    if (node_ref == NODE_REF_NIL) return SPIKEDB_OK;

    DescentResult d;
    if (descend_ex(db, root_pg, first_time, first_seq, true, &d) != SPIKEDB_OK)
        return SPIKEDB_ERROR;

    int      level = 0;
    uint64_t fwd[SPIKEDB_MAX_LEVEL];
    {
        uint32_t np;
        SkipNode* nn = node_load(db, node_ref, &np);
        if (!nn) return SPIKEDB_ERROR;
        level = nn->level;
        memcpy(fwd, nn->forward, sizeof(fwd));
        nn->leaf_page = SPIKEDB_INVALID_PAGE;
        page_dirty(db, np);
        page_unpin(db, np);
    }

    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;

    for (int i = 0; i < level; i++) {
        uint64_t pred = d.pred_ref[i];
        if (pred == NODE_REF_NIL) {
            if (root->head_forward[i] == node_ref) root->head_forward[i] = fwd[i];
        } else {
            uint32_t pp;
            SkipNode* pn = node_load(db, pred, &pp);
            if (!pn) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
            if (pn->forward[i] == node_ref) {
                pn->forward[i] = fwd[i];
                page_dirty(db, pp);
            }
            page_unpin(db, pp);
        }
    }

    uint8_t mxlvl = 1;
    for (int i = SPIKEDB_MAX_LEVEL - 1; i >= 0; i--) {
        if (root->head_forward[i] != NODE_REF_NIL) { mxlvl = (uint8_t)(i + 1); break; }
    }
    root->current_max_level = mxlvl;
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);
    return SPIKEDB_OK;
}

/* Drop an emptied leaf from the index, the leaf chain and the file. */
static SpikeDB_Status leaf_unlink(SpikeDB* db, uint32_t root_pg, uint32_t leaf_pg) {
    uint32_t prev, next;
    uint64_t node_ref;
    {
        uint8_t* lb = page_pin(db, leaf_pg);
        if (!lb) return SPIKEDB_ERROR;
        LeafHeader* lh = (LeafHeader*)lb;
        prev     = lh->prev_leaf;
        next     = lh->next_leaf;
        node_ref = lh->skiplist_node_ref;
        page_unpin(db, leaf_pg);
    }

    if (node_ref != NODE_REF_NIL) {
        uint64_t ft; uint32_t fs;
        uint32_t np;
        SkipNode* nn = node_load(db, node_ref, &np);
        if (!nn) return SPIKEDB_ERROR;
        ft = nn->first_time;
        fs = nn->first_seq;
        page_unpin(db, np);
        if (skiplist_unlink(db, root_pg, node_ref, ft, fs) != SPIKEDB_OK)
            return SPIKEDB_ERROR;
    }

    if (prev != SPIKEDB_INVALID_PAGE) {
        uint8_t* pb = page_pin(db, prev);
        if (!pb) return SPIKEDB_ERROR;
        ((LeafHeader*)pb)->next_leaf = next;
        page_dirty(db, prev);
        page_unpin(db, prev);
    }
    if (next != SPIKEDB_INVALID_PAGE) {
        uint8_t* nb = page_pin(db, next);
        if (!nb) return SPIKEDB_ERROR;
        ((LeafHeader*)nb)->prev_leaf = prev;
        page_dirty(db, next);
        page_unpin(db, next);
    }

    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    if (root->first_leaf == leaf_pg) root->first_leaf = next;
    if (root->last_leaf  == leaf_pg) root->last_leaf  = prev;
    if (root->leaf_count) root->leaf_count--;
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);

    return page_free(db, leaf_pg);
}

static SpikeDB_Status symbol_delete(SpikeDB* db, uint32_t root_pg,
                                    uint64_t time, uint32_t seq) {
    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint32_t leaf_pg = d.leaf_page;
    if (leaf_pg == SPIKEDB_INVALID_PAGE) return SPIKEDB_NOT_FOUND;

    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) return SPIKEDB_ERROR;
    LeafHeader* lh = (LeafHeader*)lb;
    bool exact;
    uint32_t pos = leaf_find(lb, time, seq, &exact);
    if (!exact) { page_unpin(db, leaf_pg); return SPIKEDB_NOT_FOUND; }

    leaf_erase(lb, pos);

    bool     now_empty = (lh->record_count == 0);
    uint64_t new_first_time = 0;
    uint32_t new_first_seq  = 0;
    leaf_refresh_bounds(lb);
    if (!now_empty) {
        leaf_key_at(lb, 0, &new_first_time, &new_first_seq);
        (void)leaf_compact(db, lb);
    }
    uint64_t node_ref = lh->skiplist_node_ref;
    page_dirty(db, leaf_pg);
    page_unpin(db, leaf_pg);

    if (now_empty) {
        if (leaf_unlink(db, root_pg, leaf_pg) != SPIKEDB_OK) return SPIKEDB_ERROR;
    } else if (pos == 0 && node_ref != NODE_REF_NIL) {
        /* The leaf's first key moved; its index entry has to follow. */
        uint32_t np;
        SkipNode* nn = node_load(db, node_ref, &np);
        if (!nn) return SPIKEDB_ERROR;
        nn->first_time = new_first_time;
        nn->first_seq  = new_first_seq;
        page_dirty(db, np);
        page_unpin(db, np);
    }

    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) return SPIKEDB_ERROR;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    if (root->record_count) root->record_count--;
    if (root->first_leaf == SPIKEDB_INVALID_PAGE) {
        root->min_time = UINT64_MAX;
        root->max_time = 0;
    } else {
        uint8_t* fb = page_pin(db, root->first_leaf);
        if (!fb) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
        root->min_time = ((LeafHeader*)fb)->min_time;
        page_unpin(db, root->first_leaf);
        uint8_t* xb = page_pin(db, root->last_leaf);
        if (!xb) { page_unpin(db, root_pg); return SPIKEDB_ERROR; }
        root->max_time = ((LeafHeader*)xb)->max_time;
        page_unpin(db, root->last_leaf);
    }
    page_dirty(db, root_pg);
    page_unpin(db, root_pg);
    return SPIKEDB_OK;
}

/* Smallest key >= (time, seq) for this symbol. */
static SpikeDB_Status find_first_ge(SpikeDB* db, uint32_t root_pg,
                                    uint64_t time, uint32_t seq,
                                    uint64_t* time_out, uint32_t* seq_out) {
    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint32_t lp = d.leaf_page;
    while (lp != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, lp);
        if (!lb) return SPIKEDB_ERROR;
        LeafHeader* lh = (LeafHeader*)lb;
        bool exact;
        uint32_t pos = leaf_find(lb, time, seq, &exact);
        if (pos < lh->record_count) {
            leaf_key_at(lb, pos, time_out, seq_out);
            page_unpin(db, lp);
            return SPIKEDB_OK;
        }
        uint32_t nx = lh->next_leaf;
        page_unpin(db, lp);
        lp = nx;
    }
    return SPIKEDB_NOT_FOUND;
}

/*============================================================================
 * Public API: open / close
 *============================================================================*/

static SpikeDB_Status db_format_fresh(SpikeDB* db) {
    /* Layout:
     *   pages 0,1   meta
     *   pages 2..17 symdir (zeroed)
     *   bump to 18
     */
    /* Zero symdir pages */
    uint8_t* zero = (uint8_t*)calloc(1, SPIKEDB_PAGE_SIZE);
    if (!zero) return SPIKEDB_ERROR;
    for (uint32_t p = SPIKEDB_SYMDIR_START; p < SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES; p++) {
        if (page_write_checked(db, p, zero) != SPIKEDB_OK) { free(zero); return SPIKEDB_ERROR; }
    }
    free(zero);

    MetaPage* m = &db->meta_buf[0];
    memset(m, 0, sizeof(*m));
    m->magic                 = SPIKEDB_MAGIC;
    m->txn_id                = 1;
    m->total_pages_allocated = SPIKEDB_RESERVED_PAGES;
    m->freelist_head         = SPIKEDB_INVALID_PAGE;
    m->symbol_count          = 0;
    m->reader_epoch          = 1;

    db->meta_buf[1] = *m;
    db->meta_buf[1].txn_id = 0;     /* B is older */
    db->active_meta = 0;
    if (meta_write(db, 0) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (meta_write(db, 1) != SPIKEDB_OK) return SPIKEDB_ERROR;
    return io_fsync(db);
}

SpikeDB_Status spike_db_open(SpikeDB** out, const char* path,
                             uint32_t cache_pages_64k, uint32_t flags) {
    if (!out || !path) return SPIKEDB_INVAL;
    if (cache_pages_64k < 16) cache_pages_64k = 16;

    SpikeDB* db = (SpikeDB*)calloc(1, sizeof(SpikeDB));
    if (!db) return SPIKEDB_ERROR;
    db->fd = SPDB_INVALID_FD;
    db->io = &spike_db_internal_io_os;
    db->last_error.page = SPIKEDB_INVALID_PAGE;
    db->readonly = (flags & SPIKEDB_OPEN_READONLY) != 0;

    /* Open file. In read-only mode we do NOT create a missing file. */
#ifdef _WIN32
    DWORD access   = db->readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD share    = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD creation = db->readonly ? OPEN_EXISTING : OPEN_ALWAYS;
    db->fd = CreateFileA(path, access, share, NULL, creation,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (db->fd == SPDB_INVALID_FD) { free(db); return SPIKEDB_ERROR; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(db->fd, &sz)) { CloseHandle(db->fd); free(db); return SPIKEDB_ERROR; }
    db->file_size = (uint64_t)sz.QuadPart;
#else
    int fl = db->readonly ? O_RDONLY : (O_RDWR | O_CREAT);
    db->fd = open(path, fl, 0644);
    if (db->fd < 0) { free(db); return SPIKEDB_ERROR; }
    struct stat st;
    if (fstat(db->fd, &st) != 0) { close(db->fd); free(db); return SPIKEDB_ERROR; }
    db->file_size = (uint64_t)st.st_size;
#endif

    /* Cache: aligned alloc */
    db->cache_capacity = cache_pages_64k;
#ifdef _WIN32
    db->storage = (uint8_t*)_aligned_malloc((size_t)cache_pages_64k * SPIKEDB_PAGE_SIZE, 4096);
#else
    if (posix_memalign((void**)&db->storage, 4096,
                       (size_t)cache_pages_64k * SPIKEDB_PAGE_SIZE) != 0)
        db->storage = NULL;
#endif
    if (!db->storage) goto fail;

    db->slots = (CacheSlot*)calloc(cache_pages_64k, sizeof(CacheSlot));
    if (!db->slots) goto fail;
    for (uint32_t i = 0; i < cache_pages_64k; i++) {
        db->slots[i].page_id = SPIKEDB_INVALID_PAGE;
        db->slots[i].bytes   = db->storage + (size_t)i * SPIKEDB_PAGE_SIZE;
    }

    /* Hash table sized 2x cache, power of 2 */
    uint32_t htsize = 1;
    while (htsize < cache_pages_64k * 2u) htsize <<= 1;
    db->ht_mask = htsize - 1;
    db->ht = (uint32_t*)malloc(htsize * sizeof(uint32_t));
    if (!db->ht) goto fail;
    for (uint32_t i = 0; i < htsize; i++) db->ht[i] = HT_EMPTY;

    /* Read or initialize meta */
    if (db->file_size < SPIKEDB_RESERVED_PAGES * (uint64_t)SPIKEDB_PAGE_SIZE) {
        if (db->readonly) goto fail;
        if (db_format_fresh(db) != SPIKEDB_OK) goto fail;
    } else {
        MetaPage a, b;
        if (io_read(db, SPIKEDB_META_A_PAGE, &a) != SPIKEDB_OK) goto fail;
        if (io_read(db, SPIKEDB_META_B_PAGE, &b) != SPIKEDB_OK) goto fail;
        SpikeDB_Status va = meta_validate(&a);
        SpikeDB_Status vb = meta_validate(&b);
        if (va != SPIKEDB_OK && vb != SPIKEDB_OK) goto fail;
        if (va != SPIKEDB_OK) { db->meta_buf[1] = b; db->active_meta = 1; }
        else if (vb != SPIKEDB_OK) { db->meta_buf[0] = a; db->active_meta = 0; }
        else if (a.txn_id >= b.txn_id) { db->meta_buf[0] = a; db->meta_buf[1] = b; db->active_meta = 0; }
        else { db->meta_buf[0] = a; db->meta_buf[1] = b; db->active_meta = 1; }
    }

    *out = db;
    SPDB_AUDIT(db);
    return SPIKEDB_OK;

fail:
    spike_db_close(db);
    return SPIKEDB_ERROR;
}

SpikeDB_Status spike_db_open_ex(SpikeDB** out, const char* path,
                                const SpikeDB_Options* opts) {
    if (!out || !path || !opts) return SPIKEDB_INVAL;
    if (opts->struct_size < sizeof(uint32_t) * 3) return SPIKEDB_INVAL;
    return spike_db_open(out, path, opts->cache_pages_64k, opts->flags);
}

uint32_t spike_db_version(void)        { return 0 * 10000 + 7 * 100 + 0; }
uint32_t spike_db_format_version(void) { return (uint32_t)(SPIKEDB_MAGIC & 0xFFu); }

void spike_db_close(SpikeDB* db) {
    if (!db) return;
    if (db->in_txn) txn_rollback(db);
    if (db->fd != SPDB_INVALID_FD) {
#ifdef _WIN32
        CloseHandle(db->fd);
#else
        close(db->fd);
#endif
    }
    free(db->ht);
    free(db->slots);
    if (db->storage) {
#ifdef _WIN32
        _aligned_free(db->storage);
#else
        free(db->storage);
#endif
    }
    free(db->txn_allocated);
    if (db->scratch_page) {
#ifdef _WIN32
        _aligned_free(db->scratch_page);
#else
        free(db->scratch_page);
#endif
    }
    free(db);
}

/*============================================================================
 * Public API: point lookup
 *============================================================================*/

/* dir: 0 = exact key, -1 = greatest key <= target, +1 = smallest key >= target */
static SpikeDB_Status lookup_key(SpikeDB* db, uint64_t symbol,
                                 uint64_t time, uint32_t seq, int dir,
                                 uint64_t* time_out, uint32_t* seq_out,
                                 void** value_out, size_t* len_out) {
    if (!db || !value_out || !len_out) return SPIKEDB_INVAL;
    *value_out = NULL; *len_out = 0;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
    uint32_t leaf_pg = d.leaf_page;
    if (leaf_pg == SPIKEDB_INVALID_PAGE) { file_unlock(db); return SPIKEDB_NOT_FOUND; }

    /* Neighbouring leaves lie entirely on one side of the target, so the
     * same search on them lands on slot 0 / the last slot as required. */
    st = SPIKEDB_NOT_FOUND;
    while (leaf_pg != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, leaf_pg);
        if (!lb) { file_unlock(db); return SPIKEDB_ERROR; }
        LeafHeader* lh = (LeafHeader*)lb;
        bool exact;
        uint32_t pos = leaf_find(lb, time, seq, &exact);

        uint32_t hit = UINT32_MAX;
        if (dir == 0)      { if (exact) hit = pos; }
        else if (dir > 0)  { if (pos < lh->record_count) hit = pos; }
        else               { if (exact) hit = pos; else if (pos > 0) hit = pos - 1; }

        if (hit != UINT32_MAX) {
            uint32_t vl;
            const uint8_t* val = leaf_val_at(lb, hit, &vl);
            void* buf = malloc(vl ? vl : 1);
            if (!buf) { page_unpin(db, leaf_pg); file_unlock(db); return SPIKEDB_ERROR; }
            memcpy(buf, val, vl);
            leaf_key_at(lb, hit, time_out ? time_out : &time, seq_out ? seq_out : &seq);
            *value_out = buf;
            *len_out   = vl;
            st = SPIKEDB_OK;
            page_unpin(db, leaf_pg);
            break;
        }

        uint32_t nxt = (dir > 0) ? lh->next_leaf : lh->prev_leaf;
        page_unpin(db, leaf_pg);
        if (dir == 0) break;
        leaf_pg = nxt;
    }

    file_unlock(db);
    return st;
}

SpikeDB_Status spike_db_get(SpikeDB* db, uint64_t symbol, uint64_t time,
                            void** value_out, size_t* len_out) {
    return lookup_key(db, symbol, time, 0, 0, NULL, NULL, value_out, len_out);
}

SpikeDB_Status spike_db_get_seq(SpikeDB* db, uint64_t symbol, uint64_t time,
                                uint32_t seq,
                                void** value_out, size_t* len_out) {
    return lookup_key(db, symbol, time, seq, 0, NULL, NULL, value_out, len_out);
}

SpikeDB_Status spike_db_get_le(SpikeDB* db, uint64_t symbol, uint64_t time,
                               uint64_t* time_out, uint32_t* seq_out,
                               void** value_out, size_t* len_out) {
    return lookup_key(db, symbol, time, UINT32_MAX, -1,
                      time_out, seq_out, value_out, len_out);
}

SpikeDB_Status spike_db_get_ge(SpikeDB* db, uint64_t symbol, uint64_t time,
                               uint64_t* time_out, uint32_t* seq_out,
                               void** value_out, size_t* len_out) {
    return lookup_key(db, symbol, time, 0, +1,
                      time_out, seq_out, value_out, len_out);
}

static void cursor_seek(SpikeDB* db, uint64_t symbol,
                        uint64_t time, uint32_t seq, uint64_t time_hi,
                        uint32_t* out_leaf, uint32_t* out_slot,
                        uint64_t* out_key_time, uint32_t* out_key_seq);

void spike_db_free(void* ptr) { free(ptr); }

/*============================================================================
 * Public API: fixed-width symbols
 *============================================================================*/

SpikeDB_Status spike_db_symbol_define(SpikeDB* db, uint64_t symbol,
                                      uint32_t record_size) {
    if (!db) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;
    if (record_size == 0 || record_size > LEAF_FIXED_MAX_REC) return SPIKEDB_INVAL;
    if (LEAF_FIXED_CAP(record_size) < 4) return SPIKEDB_INVAL;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    SpikeDB_Status st = SPIKEDB_OK;
    uint32_t root_pg;
    if (symdir_lookup(db, symbol, &root_pg, true) != SPIKEDB_OK) {
        st = SPIKEDB_FULL;
        goto fail;
    }
    {
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) { st = SPIKEDB_ERROR; goto fail; }
        SymbolRootPage* root = (SymbolRootPage*)rb;
        if (root->record_size == record_size) {
            page_unpin(db, root_pg);            /* idempotent */
        } else if (root->record_size != 0
                   || root->record_count > 0
                   || root->first_leaf != SPIKEDB_INVALID_PAGE) {
            /* Declared once. Silently switching would let two components
             * disagree about the record size without anyone noticing. */
            page_unpin(db, root_pg);
            st = SPIKEDB_INVAL;
            goto fail;
        } else {
            root->record_size = record_size;
            page_dirty(db, root_pg);
            page_unpin(db, root_pg);
        }
    }

    {
        SpikeDB_Status cs = txn_commit(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return cs;
    }

fail:
    {
        bool oom = db->cache_oom;
        txn_rollback(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return oom ? SPIKEDB_FULL : st;
    }
}

SpikeDB_Status spike_db_read_range(SpikeDB* db, uint64_t symbol,
                                   uint64_t time_lo, uint64_t time_hi,
                                   void* dst, size_t dst_records,
                                   size_t* count_out) {
    if (!db || !count_out || time_hi < time_lo) return SPIKEDB_INVAL;
    if (dst_records && !dst) return SPIKEDB_INVAL;
    *count_out = 0;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    uint32_t R;
    {
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) { file_unlock(db); return SPIKEDB_ERROR; }
        R = ((SymbolRootPage*)rb)->record_size;
        page_unpin(db, root_pg);
    }
    if (R == 0) { file_unlock(db); return SPIKEDB_INVAL; }   /* variable-width */

    uint32_t leaf, slot;
    cursor_seek(db, symbol, time_lo, 0, time_hi, &leaf, &slot, NULL, NULL);

    uint8_t*  out       = (uint8_t*)dst;
    size_t    copied    = 0;
    bool      counting  = (dst == NULL);
    bool      truncated = false;

    while (leaf != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, leaf);
        if (!lb) { file_unlock(db); return SPIKEDB_ERROR; }
        LeafHeader* lh = (LeafHeader*)lb;

        /* Include every seq at time_hi, up to and including UINT32_MAX. */
        bool exact;
        uint32_t end = leaf_find(lb, time_hi, UINT32_MAX, &exact);
        if (exact) end++;
        bool last = (end < lh->record_count);

        uint32_t avail = (end > slot) ? end - slot : 0;
        if (avail) {
            if (counting) {
                copied += avail;
            } else {
                size_t room = (dst_records > copied) ? dst_records - copied : 0;
                size_t take = avail;
                if (take > room) { take = room; truncated = true; }
                if (take)
                    memcpy(out + copied * R, leaf_fvals(lb) + (size_t)slot * R,
                           take * R);
                copied += take;
            }
        }

        uint32_t nx = lh->next_leaf;
        page_unpin(db, leaf);
        if (last || truncated) break;
        leaf = nx;
        slot = 0;
    }

    file_unlock(db);
    *count_out = copied;
    return truncated ? SPIKEDB_FULL : SPIKEDB_OK;
}

/*============================================================================
 * Public API: batch
 *============================================================================*/

enum { BATCH_OP_PUT = 0, BATCH_OP_UPSERT = 1, BATCH_OP_DEL = 2 };

typedef struct BatchEntry {
    uint64_t symbol;
    uint64_t time;
    uint32_t seq;
    uint32_t offset;        /* offset into batch->blob */
    uint32_t length;
    uint32_t idx;           /* submission order; keeps the sort deterministic */
    uint8_t  op;
} BatchEntry;

typedef struct BatchMeta {
    char*    key;
    uint8_t* value;
    size_t   len;
} BatchMeta;

struct SpikeDB_Batch {
    BatchEntry* entries;
    size_t      count;
    size_t      cap;
    uint8_t*    blob;
    size_t      blob_size;
    size_t      blob_cap;
    BatchMeta*  meta;
    size_t      meta_count;
    size_t      meta_cap;
};

SpikeDB_Batch* spike_db_batch_create(void) {
    SpikeDB_Batch* b = (SpikeDB_Batch*)calloc(1, sizeof(SpikeDB_Batch));
    return b;
}

static void batch_free_meta(SpikeDB_Batch* b) {
    for (size_t i = 0; i < b->meta_count; i++) {
        free(b->meta[i].key);
        free(b->meta[i].value);
    }
    b->meta_count = 0;
}

void spike_db_batch_destroy(SpikeDB_Batch* b) {
    if (!b) return;
    batch_free_meta(b);
    free(b->meta);
    free(b->entries);
    free(b->blob);
    free(b);
}

void spike_db_batch_clear(SpikeDB_Batch* b) {
    if (!b) return;
    batch_free_meta(b);
    b->count = 0;
    b->blob_size = 0;
}

size_t spike_db_batch_count(const SpikeDB_Batch* b) { return b ? b->count : 0; }

static SpikeDB_Status batch_append(SpikeDB_Batch* b, uint8_t op, uint64_t symbol,
                                   uint64_t time, uint32_t seq,
                                   const void* value, size_t len) {
    if (!b || (!value && len > 0)) return SPIKEDB_INVAL;
    if (len > 65000) return SPIKEDB_INVAL;     /* must fit in a leaf */

    if (b->count == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        BatchEntry* ne = (BatchEntry*)realloc(b->entries, nc * sizeof(BatchEntry));
        if (!ne) return SPIKEDB_ERROR;
        b->entries = ne; b->cap = nc;
    }
    if (b->blob_size + len > b->blob_cap) {
        size_t nc = b->blob_cap ? b->blob_cap * 2 : 4096;
        while (nc < b->blob_size + len) nc *= 2;
        uint8_t* nb = (uint8_t*)realloc(b->blob, nc);
        if (!nb) return SPIKEDB_ERROR;
        b->blob = nb; b->blob_cap = nc;
    }
    BatchEntry* e = &b->entries[b->count];
    e->symbol = symbol;
    e->time   = time;
    e->seq    = seq;
    e->offset = (uint32_t)b->blob_size;
    e->length = (uint32_t)len;
    e->idx    = (uint32_t)b->count;
    e->op     = op;
    b->count++;
    if (len) memcpy(b->blob + b->blob_size, value, len);
    b->blob_size += len;
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_batch_put(SpikeDB_Batch* b, uint64_t symbol, uint64_t time,
                                  const void* value, size_t len) {
    return batch_append(b, BATCH_OP_PUT, symbol, time, 0, value, len);
}

SpikeDB_Status spike_db_batch_put_seq(SpikeDB_Batch* b, uint64_t symbol,
                                      uint64_t time, uint32_t seq,
                                      const void* value, size_t len) {
    return batch_append(b, BATCH_OP_PUT, symbol, time, seq, value, len);
}

SpikeDB_Status spike_db_batch_put_ex(SpikeDB_Batch* b, uint64_t symbol,
                                     uint64_t time, uint32_t seq,
                                     const void* value, size_t len,
                                     uint32_t flags) {
    uint8_t op = (flags & SPIKEDB_PUT_OVERWRITE) ? BATCH_OP_UPSERT : BATCH_OP_PUT;
    return batch_append(b, op, symbol, time, seq, value, len);
}

SpikeDB_Status spike_db_batch_del(SpikeDB_Batch* b, uint64_t symbol,
                                  uint64_t time, uint32_t seq) {
    return batch_append(b, BATCH_OP_DEL, symbol, time, seq, NULL, 0);
}

SpikeDB_Status spike_db_batch_put_meta(SpikeDB_Batch* b, const char* key,
                                       const void* value, size_t len) {
    if (!b || !key || (!value && len > 0)) return SPIKEDB_INVAL;
    size_t klen = strlen(key);
    if (klen == 0 || klen > 255) return SPIKEDB_INVAL;
    if (len > SPIKEDB_META_CAPACITY) return SPIKEDB_INVAL;

    if (b->meta_count == b->meta_cap) {
        size_t nc = b->meta_cap ? b->meta_cap * 2 : 8;
        BatchMeta* nm = (BatchMeta*)realloc(b->meta, nc * sizeof(BatchMeta));
        if (!nm) return SPIKEDB_ERROR;
        b->meta = nm; b->meta_cap = nc;
    }
    char*    kc = (char*)malloc(klen + 1);
    uint8_t* vc = len ? (uint8_t*)malloc(len) : NULL;
    if (!kc || (len && !vc)) { free(kc); free(vc); return SPIKEDB_ERROR; }
    memcpy(kc, key, klen + 1);
    if (len) memcpy(vc, value, len);

    b->meta[b->meta_count].key   = kc;
    b->meta[b->meta_count].value = vc;
    b->meta[b->meta_count].len   = len;
    b->meta_count++;
    return SPIKEDB_OK;
}

/*----------------------------------------------------------------------------
 * User metadata lives inline in the meta page, so it commits and rolls back
 * with the transaction that wrote it. Encoding is a flat sequence of
 * [u16 klen][u16 vlen][key][value].
 *--------------------------------------------------------------------------*/

static uint32_t meta_kv_locate(const MetaPage* m, const char* key,
                               uint32_t* out_entry_len) {
    size_t klen = strlen(key);
    uint32_t o = 0;
    while (o + 4 <= m->user_meta_len) {
        uint16_t kl, vl;
        memcpy(&kl, m->user_meta + o, 2);
        memcpy(&vl, m->user_meta + o + 2, 2);
        uint32_t entry = 4u + kl + vl;
        if (o + entry > m->user_meta_len) break;
        if (kl == klen && memcmp(m->user_meta + o + 4, key, klen) == 0) {
            *out_entry_len = entry;
            return o;
        }
        o += entry;
    }
    return UINT32_MAX;
}

static SpikeDB_Status meta_kv_set(MetaPage* m, const char* key,
                                  const void* val, size_t vlen) {
    uint32_t entry_len = 0;
    uint32_t at = meta_kv_locate(m, key, &entry_len);
    if (at != UINT32_MAX) {
        memmove(m->user_meta + at, m->user_meta + at + entry_len,
                m->user_meta_len - at - entry_len);
        m->user_meta_len -= entry_len;
    }
    if (vlen == 0) return SPIKEDB_OK;              /* erase */

    size_t klen = strlen(key);
    uint32_t need = (uint32_t)(4 + klen + vlen);
    if (m->user_meta_len + need > SPIKEDB_META_CAPACITY) return SPIKEDB_FULL;

    uint8_t* p = m->user_meta + m->user_meta_len;
    uint16_t kl = (uint16_t)klen, vl = (uint16_t)vlen;
    memcpy(p, &kl, 2);
    memcpy(p + 2, &vl, 2);
    memcpy(p + 4, key, klen);
    memcpy(p + 4 + klen, val, vlen);
    m->user_meta_len += need;
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_get_meta(SpikeDB* db, const char* key,
                                 void** value_out, size_t* len_out) {
    if (!db || !key || !value_out || !len_out) return SPIKEDB_INVAL;
    *value_out = NULL; *len_out = 0;
    if (strlen(key) == 0) return SPIKEDB_INVAL;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    const MetaPage* m = &db->meta_buf[db->active_meta];
    uint32_t entry_len = 0;
    uint32_t at = meta_kv_locate(m, key, &entry_len);
    if (at == UINT32_MAX) { file_unlock(db); return SPIKEDB_NOT_FOUND; }

    uint16_t kl, vl;
    memcpy(&kl, m->user_meta + at, 2);
    memcpy(&vl, m->user_meta + at + 2, 2);
    uint8_t* buf = (uint8_t*)malloc((size_t)vl + 1);
    if (!buf) { file_unlock(db); return SPIKEDB_ERROR; }
    memcpy(buf, m->user_meta + at + 4 + kl, vl);
    buf[vl] = 0;
    *value_out = buf;
    *len_out   = vl;
    file_unlock(db);
    return SPIKEDB_OK;
}

static int batch_cmp(const void* a, const void* b) {
    const BatchEntry* x = (const BatchEntry*)a;
    const BatchEntry* y = (const BatchEntry*)b;
    if (x->symbol != y->symbol) return x->symbol < y->symbol ? -1 : 1;
    int k = key_cmp(x->time, x->seq, y->time, y->seq);
    if (k) return k;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_Batch* b) {
    return spike_db_write_ex(db, b, 0);
}

SpikeDB_Status spike_db_sync(SpikeDB* db) {
    if (!db) return SPIKEDB_INVAL;
    return io_fsync(db);
}

SpikeDB_Status spike_db_write_ex(SpikeDB* db, SpikeDB_Batch* b, uint32_t flags) {
    if (!db || !b) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;
    if (b->count == 0 && b->meta_count == 0) return SPIKEDB_OK;

    bool sync = (flags & SPIKEDB_WRITE_NOSYNC) == 0;
    SpikeDB_Status fail_st = SPIKEDB_ERROR;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    /* Sort by (symbol, time, seq) to amortize symdir lookups; `idx` breaks
     * ties so entries touching one key apply in submission order. */
    if (b->count) qsort(b->entries, b->count, sizeof(BatchEntry), batch_cmp);

    /* Because entries are grouped by symbol, symdir_lookup happens once
     * per distinct symbol. Use a sentinel != first symbol to force the
     * very first lookup. */
    uint64_t cur_sym = b->count ? b->entries[0].symbol + 1 : 0;
    uint32_t root_pg = SPIKEDB_INVALID_PAGE;

    for (size_t i = 0; i < b->count; i++) {
        BatchEntry* e = &b->entries[i];
        if (e->symbol != cur_sym) {
            SpikeDB_Status ls = symdir_lookup(db, e->symbol, &root_pg, false);
            if (ls == SPIKEDB_NOT_FOUND) root_pg = SPIKEDB_INVALID_PAGE;
            else if (ls != SPIKEDB_OK) { fail_st = ls; goto rollback; }
            cur_sym = e->symbol;
        }
        if (root_pg == SPIKEDB_INVALID_PAGE) {
            if (e->op == BATCH_OP_DEL) continue;   /* nothing to remove */
            SpikeDB_Status ls = symdir_lookup(db, e->symbol, &root_pg, true);
            if (ls != SPIKEDB_OK) { fail_st = ls; goto rollback; }
        }

        SpikeDB_Status st;
        if (e->op == BATCH_OP_DEL) {
            st = symbol_delete(db, root_pg, e->time, e->seq);
            if (st == SPIKEDB_NOT_FOUND) st = SPIKEDB_OK;   /* idempotent */
        } else {
            bool replaced;
            st = symbol_insert(db, root_pg, e->time, e->seq,
                               b->blob + e->offset, e->length,
                               e->op == BATCH_OP_UPSERT, &replaced);
        }
        if (st != SPIKEDB_OK) { fail_st = st; goto rollback; }
    }

    for (size_t i = 0; i < b->meta_count; i++) {
        SpikeDB_Status ms = meta_kv_set(&db->meta_buf[db->active_meta],
                                        b->meta[i].key, b->meta[i].value,
                                        b->meta[i].len);
        if (ms != SPIKEDB_OK) { fail_st = ms; goto rollback; }
    }

    {
        SpikeDB_Status cs = txn_commit_ex(db, sync);
        file_unlock(db);
        SPDB_AUDIT(db);
        return cs;
    }

rollback:
    {
        bool oom = db->cache_oom;
        txn_rollback(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return oom ? SPIKEDB_FULL : fail_st;
    }
}

/*============================================================================
 * Public API: scan iterator
 *
 * Records are copied out of the leaves a chunk at a time. In non-blocking
 * mode the shared lock is dropped between chunks, so a writer waits for
 * one chunk rather than the whole iteration; the cursor then re-descends
 * from the last key it emitted whenever the txn id has moved on.
 *============================================================================*/

#define SPDB_ITER_CHUNK_RECS   1024u
#define SPDB_ITER_CHUNK_BYTES  (256u * 1024u)
#define SPDB_ITER_REC_HDR      24u      /* u64 symbol, u64 time, u32 seq, u32 len */

/* One per symbol in a merged scan. */
typedef struct IterSub {
    uint64_t symbol;
    uint32_t leaf;                  /* INVALID = finished */
    uint32_t slot;
    uint64_t key_time;              /* key of the record at (leaf, slot) */
    uint32_t key_seq;
} IterSub;

struct SpikeDB_Iter {
    SpikeDB*  db;
    uint64_t  symbol;
    uint64_t  time_lo;
    uint64_t  time_hi;
    uint32_t  cur_leaf;             /* INVALID = walk finished (single mode) */
    uint32_t  cur_slot;
    uint8_t*  buf;                  /* chunk of packed records */
    size_t    buf_cap;
    size_t    buf_len;
    size_t    buf_pos;
    bool      locked;               /* shared lock held for the whole lifetime */
    bool      nonblocking;
    bool      reverse;
    bool      multi;
    bool      exhausted;
    bool      started;              /* last_* are meaningful */
    uint64_t  last_time;
    uint32_t  last_seq;
    uint64_t  last_symbol;
    uint64_t  seen_txn;

    IterSub*  subs;                 /* multi mode */
    uint32_t* heap;                 /* indices into subs, min-heap on key */
    uint32_t  sub_count;
    uint32_t  heap_len;
};

/* Find the first record of `symbol` with key >= (time, seq) that is still
 * at or below `time_hi`. Caller holds the shared lock. */
static void cursor_seek(SpikeDB* db, uint64_t symbol,
                        uint64_t time, uint32_t seq, uint64_t time_hi,
                        uint32_t* out_leaf, uint32_t* out_slot,
                        uint64_t* out_key_time, uint32_t* out_key_seq) {
    *out_leaf = SPIKEDB_INVALID_PAGE;
    *out_slot = 0;

    uint32_t root_pg;
    if (symdir_lookup(db, symbol, &root_pg, false) != SPIKEDB_OK) return;

    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) return;
    uint32_t lp = d.leaf_page;
    if (lp == SPIKEDB_INVALID_PAGE) {
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) return;
        lp = ((SymbolRootPage*)rb)->first_leaf;
        page_unpin(db, root_pg);
    }

    /* Every key on a later leaf is greater than the target, so re-running
     * the same search there lands on slot 0. */
    while (lp != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, lp);
        if (!lb) return;
        LeafHeader* lh = (LeafHeader*)lb;
        bool exact;
        uint32_t pos = leaf_find(lb, time, seq, &exact);
        if (pos < lh->record_count) {
            uint64_t kt; uint32_t ks;
            leaf_key_at(lb, pos, &kt, &ks);
            if (kt <= time_hi) {
                *out_leaf = lp;
                *out_slot = pos;
                if (out_key_time) *out_key_time = kt;
                if (out_key_seq)  *out_key_seq  = ks;
            }
            page_unpin(db, lp);
            return;
        }
        uint32_t nx = lh->next_leaf;
        page_unpin(db, lp);
        lp = nx;
    }
}

static void iter_position(SpikeDB_Iter* it, uint64_t time, uint32_t seq) {
    cursor_seek(it->db, it->symbol, time, seq, it->time_hi,
                &it->cur_leaf, &it->cur_slot, NULL, NULL);
}

/* Greatest key <= (time, seq) that is still at or above time_lo. */
static void cursor_seek_rev(SpikeDB* db, uint64_t symbol,
                            uint64_t time, uint32_t seq, uint64_t time_lo,
                            uint32_t* out_leaf, uint32_t* out_slot) {
    *out_leaf = SPIKEDB_INVALID_PAGE;
    *out_slot = 0;

    uint32_t root_pg;
    if (symdir_lookup(db, symbol, &root_pg, false) != SPIKEDB_OK) return;

    DescentResult d;
    if (descend(db, root_pg, time, seq, &d) != SPIKEDB_OK) return;
    uint32_t lp = d.leaf_page;

    /* Every key on an earlier leaf is below the target, so re-running the
     * same search there lands past its last record. */
    while (lp != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, lp);
        if (!lb) return;
        LeafHeader* lh = (LeafHeader*)lb;
        bool exact;
        uint32_t pos = leaf_find(lb, time, seq, &exact);
        uint32_t hit = exact ? pos : (pos > 0 ? pos - 1 : UINT32_MAX);
        if (hit != UINT32_MAX && hit < lh->record_count) {
            uint64_t kt; uint32_t ks;
            leaf_key_at(lb, hit, &kt, &ks);
            if (kt >= time_lo) {
                *out_leaf = lp;
                *out_slot = hit;
            }
            page_unpin(db, lp);
            return;
        }
        uint32_t pv = lh->prev_leaf;
        page_unpin(db, lp);
        lp = pv;
    }
}

static void iter_position_rev(SpikeDB_Iter* it, uint64_t time, uint32_t seq) {
    cursor_seek_rev(it->db, it->symbol, time, seq, it->time_lo,
                    &it->cur_leaf, &it->cur_slot);
}

/* Append one record to the chunk. Returns false only on allocation failure. */
static bool chunk_append(SpikeDB_Iter* it, uint64_t symbol,
                         const uint8_t* leaf, uint32_t idx) {
    uint64_t t; uint32_t s, vl;
    leaf_key_at(leaf, idx, &t, &s);
    const uint8_t* val = leaf_val_at(leaf, idx, &vl);

    size_t need = SPDB_ITER_REC_HDR + vl;
    if (it->buf_len + need > it->buf_cap) {
        size_t nc = it->buf_cap ? it->buf_cap * 2 : 65536;
        while (nc < it->buf_len + need) nc *= 2;
        uint8_t* nb = (uint8_t*)realloc(it->buf, nc);
        if (!nb) return false;
        it->buf = nb;
        it->buf_cap = nc;
    }
    uint8_t* p = it->buf + it->buf_len;
    memcpy(p,      &symbol, 8);
    memcpy(p + 8,  &t,      8);
    memcpy(p + 16, &s,      4);
    memcpy(p + 20, &vl,     4);
    memcpy(p + 24, val,     vl);
    it->buf_len += need;
    return true;
}

/* Copy the next chunk of in-range records into it->buf. Caller holds the
 * shared lock. Returns true if anything was copied. */
static bool iter_fill_rev(SpikeDB_Iter* it) {
    SpikeDB* db = it->db;
    it->buf_len = 0;
    it->buf_pos = 0;
    uint32_t count = 0;

    while (it->cur_leaf != SPIKEDB_INVALID_PAGE
           && count < SPDB_ITER_CHUNK_RECS
           && it->buf_len < SPDB_ITER_CHUNK_BYTES) {
        uint8_t* lb = page_pin(db, it->cur_leaf);
        if (!lb) { it->cur_leaf = SPIKEDB_INVALID_PAGE; break; }
        LeafHeader* lh = (LeafHeader*)lb;
        uint32_t    pv = lh->prev_leaf;
        bool        past_end = false;
        bool        leaf_done = false;

        if (it->cur_slot >= lh->record_count)
            it->cur_slot = lh->record_count ? lh->record_count - 1 : 0;

        for (;;) {
            if (lh->record_count == 0) { leaf_done = true; break; }
            uint64_t t; uint32_t s;
            leaf_key_at(lb, it->cur_slot, &t, &s);
            if (t < it->time_lo) { past_end = true; break; }
            if (!chunk_append(it, it->symbol, lb, it->cur_slot)) { past_end = true; break; }
            count++;
            if (it->cur_slot == 0) { leaf_done = true; break; }
            it->cur_slot--;
            if (count >= SPDB_ITER_CHUNK_RECS || it->buf_len >= SPDB_ITER_CHUNK_BYTES)
                break;
        }
        page_unpin(db, it->cur_leaf);

        if (past_end) { it->cur_leaf = SPIKEDB_INVALID_PAGE; break; }
        if (leaf_done) { it->cur_leaf = pv; it->cur_slot = UINT32_MAX; }
        else break;                       /* chunk filled mid-leaf */
    }

    if (it->cur_leaf == SPIKEDB_INVALID_PAGE) it->exhausted = true;
    return it->buf_len > 0;
}

static bool iter_fill(SpikeDB_Iter* it) {
    SpikeDB* db = it->db;
    it->buf_len = 0;
    it->buf_pos = 0;
    uint32_t count = 0;

    while (it->cur_leaf != SPIKEDB_INVALID_PAGE
           && count < SPDB_ITER_CHUNK_RECS
           && it->buf_len < SPDB_ITER_CHUNK_BYTES) {
        uint8_t* lb = page_pin(db, it->cur_leaf);
        if (!lb) { it->cur_leaf = SPIKEDB_INVALID_PAGE; break; }
        LeafHeader* lh = (LeafHeader*)lb;
        uint32_t    rc = lh->record_count;
        uint32_t    nx = lh->next_leaf;
        bool        past_end = false;

        while (it->cur_slot < rc) {
            uint64_t t; uint32_t s;
            leaf_key_at(lb, it->cur_slot, &t, &s);
            if (t > it->time_hi) { past_end = true; break; }
            if (!chunk_append(it, it->symbol, lb, it->cur_slot)) { past_end = true; break; }
            count++;
            it->cur_slot++;
            if (count >= SPDB_ITER_CHUNK_RECS || it->buf_len >= SPDB_ITER_CHUNK_BYTES)
                break;
        }
        page_unpin(db, it->cur_leaf);

        if (past_end) { it->cur_leaf = SPIKEDB_INVALID_PAGE; break; }
        if (it->cur_slot >= rc) { it->cur_leaf = nx; it->cur_slot = 0; }
        else break;                       /* chunk filled mid-leaf */
    }

    if (it->cur_leaf == SPIKEDB_INVALID_PAGE) it->exhausted = true;
    return it->buf_len > 0;
}

/*----------------------------------------------------------------------------
 * Merged multi-symbol scan
 *--------------------------------------------------------------------------*/

static int sub_cmp(const IterSub* a, const IterSub* b) {
    int k = key_cmp(a->key_time, a->key_seq, b->key_time, b->key_seq);
    if (k) return k;
    return a->symbol < b->symbol ? -1 : (a->symbol > b->symbol ? 1 : 0);
}

static void sub_settle(SpikeDB_Iter* it, IterSub* s);

static void heap_sift_down(SpikeDB_Iter* it, uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < it->heap_len
            && sub_cmp(&it->subs[it->heap[l]], &it->subs[it->heap[m]]) < 0) m = l;
        if (r < it->heap_len
            && sub_cmp(&it->subs[it->heap[r]], &it->subs[it->heap[m]]) < 0) m = r;
        if (m == i) return;
        uint32_t t = it->heap[i]; it->heap[i] = it->heap[m]; it->heap[m] = t;
        i = m;
    }
}

static void heap_pop(SpikeDB_Iter* it) {
    it->heap_len--;
    if (it->heap_len) {
        it->heap[0] = it->heap[it->heap_len];
        heap_sift_down(it, 0);
    }
}

/* Re-seek every cursor to the first record at or after (time, seq) and
 * rebuild the heap. With `skip_ties`, records whose key is exactly
 * (time, seq) are dropped for symbols at or below `after_symbol` — those
 * were already emitted, and several symbols sharing one timestamp is the
 * normal case rather than the exception. Caller holds the shared lock. */
static void multi_reposition(SpikeDB_Iter* it, uint64_t time, uint32_t seq,
                             bool skip_ties, uint64_t after_symbol) {
    it->heap_len = 0;
    for (uint32_t i = 0; i < it->sub_count; i++) {
        IterSub* s = &it->subs[i];
        cursor_seek(it->db, s->symbol, time, seq, it->time_hi,
                    &s->leaf, &s->slot, &s->key_time, &s->key_seq);
        if (s->leaf != SPIKEDB_INVALID_PAGE && skip_ties
            && s->key_time == time && s->key_seq == seq
            && s->symbol <= after_symbol) {
            s->slot++;
            sub_settle(it, s);
        }
        if (s->leaf != SPIKEDB_INVALID_PAGE) it->heap[it->heap_len++] = i;
    }
    for (int32_t i = (int32_t)it->heap_len / 2 - 1; i >= 0; i--)
        heap_sift_down(it, (uint32_t)i);
}

/* Move a cursor to the first in-range record at or after (leaf, slot),
 * following the leaf chain. Caller holds the shared lock. */
static void sub_settle(SpikeDB_Iter* it, IterSub* s) {
    SpikeDB* db = it->db;
    while (s->leaf != SPIKEDB_INVALID_PAGE) {
        uint32_t pinned = s->leaf;
        uint8_t* lb = page_pin(db, pinned);
        if (!lb) { s->leaf = SPIKEDB_INVALID_PAGE; return; }
        LeafHeader* lh = (LeafHeader*)lb;
        if (s->slot < lh->record_count) {
            uint64_t kt; uint32_t ks;
            leaf_key_at(lb, s->slot, &kt, &ks);
            if (kt > it->time_hi) s->leaf = SPIKEDB_INVALID_PAGE;
            else { s->key_time = kt; s->key_seq = ks; }
            page_unpin(db, pinned);
            return;
        }
        uint32_t nx = lh->next_leaf;
        page_unpin(db, pinned);
        s->leaf = nx;
        s->slot = 0;
    }
}

static bool iter_fill_multi(SpikeDB_Iter* it) {
    SpikeDB* db = it->db;
    it->buf_len = 0;
    it->buf_pos = 0;
    uint32_t count = 0;

    while (it->heap_len > 0
           && count < SPDB_ITER_CHUNK_RECS
           && it->buf_len < SPDB_ITER_CHUNK_BYTES) {
        IterSub* s = &it->subs[it->heap[0]];

        uint32_t pinned = s->leaf;
        uint8_t* lb = page_pin(db, pinned);
        if (!lb) { s->leaf = SPIKEDB_INVALID_PAGE; heap_pop(it); continue; }
        LeafHeader* lh = (LeafHeader*)lb;
        uint32_t    rc = lh->record_count;
        uint32_t    nx = lh->next_leaf;

        if (!chunk_append(it, s->symbol, lb, s->slot)) {
            page_unpin(db, pinned);
            break;
        }
        count++;
        s->slot++;

        /* Staying inside the same leaf is the common case and needs no
         * second pin. */
        bool settled = false;
        if (s->slot < rc) {
            uint64_t kt; uint32_t ks;
            leaf_key_at(lb, s->slot, &kt, &ks);
            if (kt > it->time_hi) s->leaf = SPIKEDB_INVALID_PAGE;
            else { s->key_time = kt; s->key_seq = ks; }
            settled = true;
        }
        page_unpin(db, pinned);

        if (!settled) {
            s->leaf = nx;
            s->slot = 0;
            sub_settle(it, s);
        }

        if (s->leaf == SPIKEDB_INVALID_PAGE) heap_pop(it);
        else heap_sift_down(it, 0);
    }

    if (it->heap_len == 0) it->exhausted = true;
    return it->buf_len > 0;
}

static bool iter_fill_any(SpikeDB_Iter* it) {
    if (it->multi)   return iter_fill_multi(it);
    if (it->reverse) return iter_fill_rev(it);
    return iter_fill(it);
}

/* Step one key past the last one emitted, in the iterator's direction.
 * Returns false when the key space is exhausted at that end. */
static bool iter_resume_key(const SpikeDB_Iter* it, uint64_t* t, uint32_t* s) {
    if (!it->started) {
        *t = it->reverse ? it->time_hi : it->time_lo;
        *s = it->reverse ? UINT32_MAX : 0;
        return true;
    }
    *t = it->last_time;
    *s = it->last_seq;
    if (it->reverse) {
        if (*s == 0) {
            if (*t == 0) return false;
            (*t)--; *s = UINT32_MAX;
        } else (*s)--;
    } else {
        if (*s == UINT32_MAX) {
            if (*t == UINT64_MAX) return false;
            (*t)++; *s = 0;
        } else (*s)++;
    }
    return true;
}

static bool iter_refill(SpikeDB_Iter* it) {
    if (it->exhausted) return false;
    if (!it->nonblocking) return iter_fill_any(it);

    SpikeDB* db = it->db;
    if (file_lock(db, false) != SPIKEDB_OK) { it->exhausted = true; return false; }
    if (db_refresh_meta(db) != SPIKEDB_OK) {
        file_unlock(db); it->exhausted = true; return false;
    }

    uint64_t txn = db->meta_buf[db->active_meta].txn_id;
    if (txn != it->seen_txn) {
        /* Leaves may have split, merged away or moved; re-descend from
         * just past the last key handed to the caller. */
        it->seen_txn = txn;
        if (it->multi) {
            if (it->started)
                multi_reposition(it, it->last_time, it->last_seq, true, it->last_symbol);
            else
                multi_reposition(it, it->time_lo, 0, false, 0);
        } else {
            uint64_t rt; uint32_t rs;
            if (!iter_resume_key(it, &rt, &rs)) {
                file_unlock(db); it->exhausted = true; return false;
            }
            if (it->reverse) iter_position_rev(it, rt, rs);
            else             iter_position(it, rt, rs);
        }
    }

    bool got = iter_fill_any(it);
    file_unlock(db);
    return got;
}

SpikeDB_Iter* spike_db_scan_ex(SpikeDB* db, uint64_t symbol,
                               uint64_t time_lo, uint64_t time_hi,
                               uint32_t flags) {
    if (!db || time_hi < time_lo) return NULL;
    SpikeDB_Iter* it = (SpikeDB_Iter*)calloc(1, sizeof(SpikeDB_Iter));
    if (!it) return NULL;
    it->db = db; it->symbol = symbol; it->time_lo = time_lo; it->time_hi = time_hi;
    it->cur_leaf    = SPIKEDB_INVALID_PAGE;
    it->nonblocking = (flags & SPIKEDB_SCAN_NONBLOCKING) != 0;
    it->reverse     = (flags & SPIKEDB_SCAN_REVERSE) != 0;

    if (file_lock(db, false) != SPIKEDB_OK) { free(it); return NULL; }
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); free(it); return NULL; }

    it->seen_txn = db->meta_buf[db->active_meta].txn_id;
    if (it->reverse) iter_position_rev(it, time_hi, UINT32_MAX);
    else             iter_position(it, time_lo, 0);
    if (it->cur_leaf == SPIKEDB_INVALID_PAGE) it->exhausted = true;

    if (it->nonblocking) file_unlock(db);
    else it->locked = true;
    return it;
}

SpikeDB_Iter* spike_db_scan(SpikeDB* db, uint64_t symbol,
                            uint64_t time_lo, uint64_t time_hi) {
    return spike_db_scan_ex(db, symbol, time_lo, time_hi, 0);
}

SpikeDB_Iter* spike_db_scan_multi(SpikeDB* db,
                                  const uint64_t* symbols, size_t count,
                                  uint64_t time_lo, uint64_t time_hi,
                                  uint32_t flags) {
    if (!db || time_hi < time_lo) return NULL;
    if (count && !symbols) return NULL;
    if (count > UINT32_MAX) return NULL;

    SpikeDB_Iter* it = (SpikeDB_Iter*)calloc(1, sizeof(SpikeDB_Iter));
    if (!it) return NULL;
    it->db = db; it->time_lo = time_lo; it->time_hi = time_hi;
    it->cur_leaf    = SPIKEDB_INVALID_PAGE;
    it->multi       = true;
    it->nonblocking = (flags & SPIKEDB_SCAN_NONBLOCKING) != 0;
    it->sub_count   = (uint32_t)count;

    if (count) {
        it->subs = (IterSub*)calloc(count, sizeof(IterSub));
        it->heap = (uint32_t*)calloc(count, sizeof(uint32_t));
        if (!it->subs || !it->heap) {
            free(it->subs); free(it->heap); free(it);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            it->subs[i].symbol = symbols[i];
            it->subs[i].leaf   = SPIKEDB_INVALID_PAGE;
        }
    } else {
        it->exhausted = true;
    }

    if (file_lock(db, false) != SPIKEDB_OK) {
        free(it->subs); free(it->heap); free(it);
        return NULL;
    }
    if (db_refresh_meta(db) != SPIKEDB_OK) {
        file_unlock(db); free(it->subs); free(it->heap); free(it);
        return NULL;
    }

    it->seen_txn = db->meta_buf[db->active_meta].txn_id;
    if (count) {
        multi_reposition(it, time_lo, 0, false, 0);
        if (it->heap_len == 0) it->exhausted = true;
    }

    if (it->nonblocking) file_unlock(db);
    else it->locked = true;
    return it;
}

/* Decode the record at buf_pos without consuming it. */
static const uint8_t* iter_peek(SpikeDB_Iter* it, uint64_t* sym, uint64_t* t,
                                uint32_t* seq, uint32_t* len) {
    const uint8_t* p = it->buf + it->buf_pos;
    memcpy(sym, p,      8);
    memcpy(t,   p + 8,  8);
    memcpy(seq, p + 16, 4);
    memcpy(len, p + 20, 4);
    return p + SPDB_ITER_REC_HDR;
}

bool spike_db_iter_next(SpikeDB_Iter* it,
                        uint64_t* time_out,
                        const void** value_out, size_t* len_out) {
    return spike_db_iter_next_seq(it, time_out, NULL, value_out, len_out);
}

bool spike_db_iter_next_seq(SpikeDB_Iter* it,
                            uint64_t* time_out, uint32_t* seq_out,
                            const void** value_out, size_t* len_out) {
    return spike_db_iter_next_multi(it, NULL, time_out, seq_out,
                                    value_out, len_out);
}

bool spike_db_iter_next_multi(SpikeDB_Iter* it,
                              uint64_t* symbol_out, uint64_t* time_out,
                              uint32_t* seq_out,
                              const void** value_out, size_t* len_out) {
    if (!it) return false;
    if (it->buf_pos >= it->buf_len && !iter_refill(it)) return false;

    uint64_t sym, t; uint32_t sq, vl;
    const uint8_t* val = iter_peek(it, &sym, &t, &sq, &vl);

    if (symbol_out) *symbol_out = sym;
    if (time_out)   *time_out   = t;
    if (seq_out)    *seq_out    = sq;
    if (value_out)  *value_out  = val;
    if (len_out)    *len_out    = vl;

    it->buf_pos  += SPDB_ITER_REC_HDR + vl;
    it->last_time   = t;
    it->last_seq    = sq;
    it->last_symbol = sym;
    it->started     = true;
    return true;
}

size_t spike_db_iter_next_batch(SpikeDB_Iter* it, SpikeDB_Rec* out, size_t max) {
    if (!it || !out || max == 0) return 0;
    if (it->buf_pos >= it->buf_len && !iter_refill(it)) return 0;

    size_t n = 0;
    while (n < max && it->buf_pos < it->buf_len) {
        uint64_t sym, t; uint32_t sq, vl;
        const uint8_t* val = iter_peek(it, &sym, &t, &sq, &vl);
        out[n].symbol = sym;
        out[n].time   = t;
        out[n].seq    = sq;
        out[n].len    = vl;
        out[n].value  = val;

        it->buf_pos  += SPDB_ITER_REC_HDR + vl;
        it->last_time   = t;
        it->last_seq    = sq;
        it->last_symbol = sym;
        n++;
    }
    it->started = true;
    return n;
}

void spike_db_iter_close(SpikeDB_Iter* it) {
    if (!it) return;
    if (it->locked) file_unlock(it->db);
    free(it->subs);
    free(it->heap);
    free(it->buf);
    free(it);
}

SpikeDB_Status spike_db_iter_seek(SpikeDB_Iter* it, uint64_t time, uint32_t seq) {
    if (!it) return SPIKEDB_INVAL;
    if (it->multi) return SPIKEDB_INVAL;

    SpikeDB* db = it->db;
    bool need_lock = it->nonblocking;
    if (need_lock) {
        if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
        if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
        it->seen_txn = db->meta_buf[db->active_meta].txn_id;
    }

    it->buf_len   = 0;
    it->buf_pos   = 0;
    it->started   = false;
    it->exhausted = false;
    if (it->reverse) iter_position_rev(it, time, seq);
    else             iter_position(it, time, seq);
    if (it->cur_leaf == SPIKEDB_INVALID_PAGE) it->exhausted = true;

    if (need_lock) file_unlock(db);
    return SPIKEDB_OK;
}

/*============================================================================
 * Public API: metadata queries
 *============================================================================*/

static SpikeDB_Status read_root_field(SpikeDB* db, uint64_t symbol,
                                      size_t field_offset, uint64_t* out) {
    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) { file_unlock(db); return SPIKEDB_ERROR; }
    *out = *(uint64_t*)(rb + field_offset);
    page_unpin(db, root_pg);
    file_unlock(db);
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_max_time(SpikeDB* db, uint64_t symbol, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    *out = 0;
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, max_time), out);
    if (st == SPIKEDB_OK && *out == 0) {
        /* check record_count to disambiguate empty */
        uint64_t cnt = 0;
        if (read_root_field(db, symbol, offsetof(SymbolRootPage, record_count), &cnt) == SPIKEDB_OK
            && cnt == 0) { *out = 0; return SPIKEDB_NOT_FOUND; }
    }
    if (st != SPIKEDB_OK) *out = 0;
    return st;
}

SpikeDB_Status spike_db_min_time(SpikeDB* db, uint64_t symbol, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    *out = 0;
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, min_time), out);
    if (st == SPIKEDB_OK && *out == UINT64_MAX) { *out = 0; return SPIKEDB_NOT_FOUND; }
    if (st != SPIKEDB_OK) *out = 0;
    return st;
}

SpikeDB_Status spike_db_count(SpikeDB* db, uint64_t symbol, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    *out = 0;
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, record_count), out);
    if (st == SPIKEDB_NOT_FOUND) { *out = 0; return SPIKEDB_OK; }
    if (st != SPIKEDB_OK) *out = 0;
    return st;
}

SpikeDB_Status spike_db_symbol_info(SpikeDB* db, uint64_t symbol,
                                    SpikeDB_SymbolInfo* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    memset(out, 0, sizeof(*out));

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) { file_unlock(db); return SPIKEDB_ERROR; }
    const SymbolRootPage* root = (const SymbolRootPage*)rb;
    out->record_count = root->record_count;
    out->leaf_count   = root->leaf_count;
    out->record_size  = root->record_size;
    if (root->record_count) {
        out->min_time = root->min_time;
        out->max_time = root->max_time;
    }
    page_unpin(db, root_pg);
    file_unlock(db);
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_max_times(SpikeDB* db, const uint64_t* symbols,
                                  uint64_t* times_out, size_t count) {
    if (!db || (count && (!symbols || !times_out))) return SPIKEDB_INVAL;
    if (count == 0) return SPIKEDB_OK;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    for (size_t i = 0; i < count; i++) {
        times_out[i] = 0;
        uint32_t root_pg;
        if (symdir_lookup(db, symbols[i], &root_pg, false) != SPIKEDB_OK) continue;
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) { file_unlock(db); return SPIKEDB_ERROR; }
        const SymbolRootPage* root = (const SymbolRootPage*)rb;
        if (root->record_count) times_out[i] = root->max_time;
        page_unpin(db, root_pg);
    }
    file_unlock(db);
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_txn_id(SpikeDB* db, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    *out = 0;
    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
    *out = db->meta_buf[db->active_meta].txn_id;
    file_unlock(db);
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_list_symbols(SpikeDB* db, uint64_t* out, size_t cap,
                                     size_t* count_out) {
    if (!db || !count_out || (cap && !out)) return SPIKEDB_INVAL;
    *count_out = 0;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t per_pg = SYMDIR_SLOTS_PER_PAGE;
    size_t   found  = 0;
    for (uint32_t pg = SPIKEDB_SYMDIR_START;
         pg < SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES; pg++) {
        uint8_t* bytes = page_pin(db, pg);
        if (!bytes) { file_unlock(db); return SPIKEDB_ERROR; }
        const SymDirSlot* slots = (const SymDirSlot*)bytes;
        for (uint32_t i = 0; i < per_pg; i++) {
            if (!symdir_slot_live(&slots[i])) continue;
            if (found < cap) out[found] = slots[i].symbol;
            found++;
        }
        page_unpin(db, pg);
    }
    file_unlock(db);
    *count_out = found;
    return SPIKEDB_OK;
}

/*============================================================================
 * Public API: truncate_before
 *============================================================================*/

/* Remove records with time < cutoff from `leaf`. Updates header. Returns
 * the number of records removed. */
static uint32_t leaf_drop_below(SpikeDB* db, uint8_t* page, uint64_t cutoff) {
    LeafHeader* h = (LeafHeader*)page;
    if (h->record_count == 0 || h->min_time >= cutoff) return 0;
    bool exact;
    uint32_t cut_idx = leaf_find(page, cutoff, 0, &exact);
    if (cut_idx == 0) return 0;
    uint32_t removed = cut_idx;
    leaf_drop_front(page, cut_idx);
    leaf_refresh_bounds(page);
    if (h->record_count) (void)leaf_compact(db, page);
    return removed;
}

SpikeDB_Status spike_db_truncate_before(SpikeDB* db, uint64_t symbol,
                                        uint64_t cutoff) {
    if (!db) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st == SPIKEDB_NOT_FOUND) { file_unlock(db); return SPIKEDB_OK; }
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint64_t removed_total = 0;

    /* Walk leaf chain, freeing entire leaves whose max_time < cutoff.
     * The first leaf that survives may need partial truncation. */
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) goto fail;
    SymbolRootPage* root = (SymbolRootPage*)rb;
    uint32_t leaf_pg = root->first_leaf;
    page_unpin(db, root_pg);

    uint32_t new_first = SPIKEDB_INVALID_PAGE;

    while (leaf_pg != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, leaf_pg);
        if (!lb) goto fail;
        LeafHeader* lh = (LeafHeader*)lb;
        uint32_t next = lh->next_leaf;
        if (lh->record_count == 0 || lh->max_time < cutoff) {
            /* Drop entire leaf */
            removed_total += lh->record_count;
            page_unpin(db, leaf_pg);
            if (page_free(db, leaf_pg) != SPIKEDB_OK) goto fail;
            leaf_pg = next;
        } else {
            /* Partial or full survival */
            uint32_t r = leaf_drop_below(db, lb, cutoff);
            removed_total += r;
            if (r > 0) page_dirty(db, leaf_pg);
            new_first = leaf_pg;
            /* Clear prev pointer of new first leaf */
            if (lh->prev_leaf != SPIKEDB_INVALID_PAGE) {
                lh->prev_leaf = SPIKEDB_INVALID_PAGE;
                page_dirty(db, leaf_pg);
            }
            page_unpin(db, leaf_pg);
            break;
        }
    }

    /* Now rebuild the skip list head: walk each level's chain, dropping
     * any node whose leaf_page is no longer referenced (i.e. < new_first
     * in the original chain). We simply walk forward[level] until we hit
     * a node whose leaf_page is `new_first` or later in the chain.
     *
     * "Later in the chain" is hard to compute without full traversal.
     * Instead: at each level we drop nodes whose leaf_page page id was
     * just freed. Since freed pages may be reused later by page_alloc
     * during this same transaction, and node_alloc_in_root may pick
     * those, freed-set checking against db->txn_allocated is unreliable.
     *
     * Simpler: walk each level; for each candidate node, pin its leaf
     * and check `lh->symbol != symbol` (leaf was freed and overwritten
     * later) OR `lh->max_time < cutoff` to decide whether to drop.
     * Because we haven't yet freed leaves that would be reused (freelist
     * is LIFO and we free leaves before allocating any nodes here),
     * leaves freed by truncate stay freed for this txn.
     */
    rb = page_pin(db, root_pg);
    if (!rb) goto fail;
    root = (SymbolRootPage*)rb;
    root->first_leaf = new_first;
    if (new_first == SPIKEDB_INVALID_PAGE) {
        root->last_leaf  = SPIKEDB_INVALID_PAGE;
        root->leaf_count = 0;
        root->min_time   = UINT64_MAX;
        root->max_time   = 0;
    } else {
        /* Recompute leaf_count, min_time */
        uint32_t cnt = 0;
        uint64_t mn  = UINT64_MAX;
        uint32_t walk = new_first;
        uint32_t last = SPIKEDB_INVALID_PAGE;
        while (walk != SPIKEDB_INVALID_PAGE) {
            uint8_t* wb = page_pin(db, walk);
            if (!wb) { page_unpin(db, root_pg); goto fail; }
            LeafHeader* wh = (LeafHeader*)wb;
            cnt++;
            if (wh->min_time < mn) mn = wh->min_time;
            last = walk;
            uint32_t nx = wh->next_leaf;
            page_unpin(db, walk);
            walk = nx;
        }
        root->leaf_count = cnt;
        root->min_time   = mn;
        root->last_leaf  = last;
    }
    root->record_count = (root->record_count > removed_total)
                       ? (root->record_count - removed_total) : 0;

    /* Walk each skip-list level, splicing out nodes whose target leaf
     * was freed (or whose max_time is below cutoff). We track only the
     * previous node_ref; nothing remains pinned between iterations. */
    for (int lvl = 0; lvl < SPIKEDB_MAX_LEVEL; lvl++) {
        uint64_t prev_ref = NODE_REF_NIL;   /* NIL = head */
        for (;;) {
            uint64_t cur_ref;
            if (prev_ref == NODE_REF_NIL) cur_ref = root->head_forward[lvl];
            else {
                uint32_t pp;
                SkipNode* pn = node_load(db, prev_ref, &pp);
                if (!pn) { page_unpin(db, root_pg); goto fail; }
                cur_ref = pn->forward[lvl];
                page_unpin(db, pp);
            }
            if (cur_ref == NODE_REF_NIL) break;

            uint32_t np;
            SkipNode* nn = node_load(db, cur_ref, &np);
            if (!nn) { page_unpin(db, root_pg); goto fail; }
            uint32_t target_leaf = nn->leaf_page;
            uint64_t next_ref    = nn->forward[lvl];
            page_unpin(db, np);

            bool drop = false;
            uint64_t new_first_time = 0;
            uint32_t new_first_seq  = 0;
            if (target_leaf == SPIKEDB_INVALID_PAGE) drop = true;
            else {
                uint8_t* tb = page_pin(db, target_leaf);
                if (!tb) drop = true;
                else {
                    LeafHeader* th = (LeafHeader*)tb;
                    if (th->symbol != symbol || th->record_count == 0
                        || th->max_time < cutoff) drop = true;
                    else {
                        leaf_key_at(tb, 0, &new_first_time, &new_first_seq);
                    }
                    page_unpin(db, target_leaf);
                }
            }

            if (drop) {
                /* Splice out: prev.forward[lvl] = next_ref */
                if (prev_ref == NODE_REF_NIL) {
                    root->head_forward[lvl] = next_ref;
                } else {
                    uint32_t pp;
                    SkipNode* pn = node_load(db, prev_ref, &pp);
                    if (!pn) { page_unpin(db, root_pg); goto fail; }
                    pn->forward[lvl] = next_ref;
                    page_dirty(db, pp);
                    page_unpin(db, pp);
                }
                /* prev_ref unchanged; loop continues */
            } else {
                /* Update first_time if needed */
                SkipNode* nn2 = node_load(db, cur_ref, &np);
                if (nn2 && (nn2->first_time != new_first_time
                            || nn2->first_seq != new_first_seq)) {
                    nn2->first_time = new_first_time;
                    nn2->first_seq  = new_first_seq;
                    page_dirty(db, np);
                }
                if (nn2) page_unpin(db, np);
                prev_ref = cur_ref;
            }
        }
    }

    /* Recompute current_max_level */
    uint8_t mxlvl = 1;
    for (int i = SPIKEDB_MAX_LEVEL - 1; i >= 0; i--) {
        if (root->head_forward[i] != NODE_REF_NIL) { mxlvl = (uint8_t)(i + 1); break; }
    }
    root->current_max_level = mxlvl;

    page_dirty(db, root_pg);
    page_unpin(db, root_pg);

    SpikeDB_Status cs = txn_commit(db);
    file_unlock(db);
    SPDB_AUDIT(db);
    return cs;

fail:
    {
        bool oom = db->cache_oom;
        txn_rollback(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return oom ? SPIKEDB_FULL : SPIKEDB_ERROR;
    }
}

/*============================================================================
 * Public API: delete_range
 *============================================================================*/

SpikeDB_Status spike_db_delete_range(SpikeDB* db, uint64_t symbol,
                                     uint64_t time_lo, uint64_t time_hi) {
    if (!db || time_hi < time_lo) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st == SPIKEDB_NOT_FOUND) { file_unlock(db); return SPIKEDB_OK; }
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    /* Each pass removes the lowest surviving key in range, so progress is
     * guaranteed and no key list has to be materialized. */
    for (;;) {
        uint64_t t; uint32_t s;
        SpikeDB_Status f = find_first_ge(db, root_pg, time_lo, 0, &t, &s);
        if (f == SPIKEDB_NOT_FOUND) break;
        if (f != SPIKEDB_OK) goto fail;
        if (t > time_hi) break;
        if (symbol_delete(db, root_pg, t, s) != SPIKEDB_OK) goto fail;
    }

    {
        SpikeDB_Status cs = txn_commit(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return cs;
    }

fail:
    {
        bool oom = db->cache_oom;
        txn_rollback(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return oom ? SPIKEDB_FULL : SPIKEDB_ERROR;
    }
}

/*============================================================================
 * Public API: symbol drop, tailing, prefetch, backup
 *============================================================================*/

SpikeDB_Status spike_db_symbol_drop(SpikeDB* db, uint64_t symbol) {
    if (!db) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    /* Locate the directory slot itself, not just the root page. */
    uint32_t pg, slot, root_pg = SPIKEDB_INVALID_PAGE;
    uint32_t hit_pg = SPIKEDB_INVALID_PAGE, hit_slot = 0;
    symdir_locate(symbol, &pg, &slot);
    for (uint64_t probes = 0; probes < SYMDIR_TOTAL_SLOTS; probes++) {
        uint8_t* bytes = page_pin(db, pg);
        if (!bytes) { file_unlock(db); return SPIKEDB_ERROR; }
        SymDirSlot* s = (SymDirSlot*)bytes + slot;
        bool empty = symdir_slot_empty(s);
        if (!empty && s->symbol == symbol && s->root_page != SYMDIR_TOMBSTONE) {
            root_pg  = s->root_page;
            hit_pg   = pg;
            hit_slot = slot;
        }
        page_unpin(db, pg);
        if (empty || root_pg != SPIKEDB_INVALID_PAGE) break;
        slot++;
        if (slot == SYMDIR_SLOTS_PER_PAGE) {
            slot = 0; pg++;
            if (pg >= SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES) pg = SPIKEDB_SYMDIR_START;
        }
    }
    if (root_pg == SPIKEDB_INVALID_PAGE) { file_unlock(db); return SPIKEDB_NOT_FOUND; }

    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t first_leaf, node_pg;
    {
        uint8_t* rb = page_pin(db, root_pg);
        if (!rb) goto fail;
        SymbolRootPage* root = (SymbolRootPage*)rb;
        first_leaf = root->first_leaf;
        node_pg    = root->current_node_page;
        page_unpin(db, root_pg);
    }

    for (uint32_t lp = first_leaf; lp != SPIKEDB_INVALID_PAGE; ) {
        uint8_t* lb = page_pin(db, lp);
        if (!lb) goto fail;
        uint32_t nx = ((LeafHeader*)lb)->next_leaf;
        page_unpin(db, lp);
        if (page_free(db, lp) != SPIKEDB_OK) goto fail;
        lp = nx;
    }
    for (uint32_t np = node_pg; np != SPIKEDB_INVALID_PAGE; ) {
        uint8_t* nb = page_pin(db, np);
        if (!nb) goto fail;
        uint32_t nx = ((NodePageHeader*)nb)->next_node_page;
        page_unpin(db, np);
        if (page_free(db, np) != SPIKEDB_OK) goto fail;
        np = nx;
    }
    if (page_free(db, root_pg) != SPIKEDB_OK) goto fail;

    {
        uint8_t* bytes = page_pin(db, hit_pg);
        if (!bytes) goto fail;
        SymDirSlot* s = (SymDirSlot*)bytes + hit_slot;
        s->symbol    = symbol;
        s->root_page = SYMDIR_TOMBSTONE;
        page_dirty(db, hit_pg);
        page_unpin(db, hit_pg);
    }
    {
        MetaPage* m = &db->meta_buf[db->active_meta];
        if (m->symbol_count) m->symbol_count--;
    }

    {
        SpikeDB_Status cs = txn_commit(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return cs;
    }

fail:
    {
        bool oom = db->cache_oom;
        txn_rollback(db);
        file_unlock(db);
        SPDB_AUDIT(db);
        return oom ? SPIKEDB_FULL : SPIKEDB_ERROR;
    }
}

static void spdb_sleep_us(uint32_t us) {
#ifdef _WIN32
    Sleep(us < 1000 ? 1 : us / 1000);
#else
    struct timespec ts;
    ts.tv_sec  = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

SpikeDB_Status spike_db_wait_for_txn(SpikeDB* db, uint64_t last_seen,
                                     uint32_t timeout_ms, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    *out = 0;

    uint32_t waited_us = 0;
    uint32_t backoff_us = 100;
    for (;;) {
        uint64_t txn;
        SpikeDB_Status st = spike_db_txn_id(db, &txn);
        if (st != SPIKEDB_OK) return st;
        if (txn != last_seen) { *out = txn; return SPIKEDB_OK; }
        if (waited_us >= (uint64_t)timeout_ms * 1000u) { *out = txn; return SPIKEDB_NOT_FOUND; }
        spdb_sleep_us(backoff_us);
        waited_us += backoff_us;
        if (backoff_us < 5000) backoff_us *= 2;
    }
}

SpikeDB_Status spike_db_prefetch(SpikeDB* db, uint64_t symbol,
                                 uint64_t time_lo, uint64_t time_hi) {
    if (!db || time_hi < time_lo) return SPIKEDB_INVAL;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t leaf, slot;
    cursor_seek(db, symbol, time_lo, 0, time_hi, &leaf, &slot, NULL, NULL);

    /* Touching more than the cache holds would just evict what we warmed. */
    uint32_t budget = db->cache_capacity > 8 ? db->cache_capacity - 8 : 1;
    while (leaf != SPIKEDB_INVALID_PAGE && budget--) {
        uint8_t* lb = page_pin(db, leaf);
        if (!lb) break;
        LeafHeader* lh = (LeafHeader*)lb;
        uint32_t nx = lh->next_leaf;
        bool done = (lh->record_count == 0) || (lh->min_time > time_hi);
        page_unpin(db, leaf);
        if (done) break;
        leaf = nx;
    }

    file_unlock(db);
    return SPIKEDB_OK;
}

SpikeDB_Status spike_db_backup(SpikeDB* db, const char* dest_path) {
    if (!db || !dest_path) return SPIKEDB_INVAL;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint64_t pages = db->meta_buf[db->active_meta].total_pages_allocated;
    uint8_t* buf = (uint8_t*)malloc(SPIKEDB_PAGE_SIZE);
    FILE*    f   = fopen(dest_path, "wb");
    if (!buf || !f) {
        free(buf);
        if (f) fclose(f);
        file_unlock(db);
        return set_err(db, SPIKEDB_ERROR, os_last_error(), SPIKEDB_INVALID_PAGE,
                       "backup destination could not be opened");
    }

    SpikeDB_Status st = SPIKEDB_OK;
    for (uint64_t p = 0; p < pages; p++) {
        if (io_read(db, (uint32_t)p, buf) != SPIKEDB_OK) { st = SPIKEDB_ERROR; break; }
        if (fwrite(buf, 1, SPIKEDB_PAGE_SIZE, f) != SPIKEDB_PAGE_SIZE) {
            st = set_err(db, SPIKEDB_ERROR, os_last_error(), (uint32_t)p,
                         "backup write failed");
            break;
        }
    }

    if (st == SPIKEDB_OK && fflush(f) != 0) st = SPIKEDB_ERROR;
    fclose(f);
    free(buf);
    file_unlock(db);
    return st;
}

/*============================================================================
 * Public API: diagnostics
 *============================================================================*/
const char* spike_db_strerror(SpikeDB_Status status) {
    switch (status) {
        case SPIKEDB_OK:        return "ok";
        case SPIKEDB_NOT_FOUND: return "not found";
        case SPIKEDB_ERROR:     return "i/o or system error";
        case SPIKEDB_FULL:      return "capacity exhausted";
        case SPIKEDB_CORRUPT:   return "corrupt or unrecognized file";
        case SPIKEDB_INVAL:     return "invalid argument";
    }
    return "unknown status";
}

void spike_db_last_error(SpikeDB* db, SpikeDB_Error* out) {
    if (!out) return;
    if (!db) {
        memset(out, 0, sizeof(*out));
        out->page = SPIKEDB_INVALID_PAGE;
        snprintf(out->message, sizeof(out->message), "no database handle");
        return;
    }
    *out = db->last_error;
    if (out->message[0] == 0)
        snprintf(out->message, sizeof(out->message), "%s", spike_db_strerror(out->status));
}

/*============================================================================
 * Public API: verify
 *============================================================================*/

static void vfy_err(SpikeDB_VerifyReport* r, const char* fmt, ...) {
    r->errors++;
    if (r->first_error[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->first_error, sizeof(r->first_error), fmt, ap);
    va_end(ap);
}

static void verify_symbol(SpikeDB* db, uint64_t symbol, uint32_t root_pg,
                          SpikeDB_VerifyReport* r) {
    uint8_t* rb = page_pin(db, root_pg);
    if (!rb) { vfy_err(r, "symbol %llu: root page %u unreadable",
                       (unsigned long long)symbol, root_pg); return; }
    SymbolRootPage root = *(SymbolRootPage*)rb;    /* copy: leaves get pinned below */
    page_unpin(db, root_pg);

    if (root.symbol != symbol)
        vfy_err(r, "symbol %llu: root page holds %llu",
                (unsigned long long)symbol, (unsigned long long)root.symbol);

    uint64_t leaves = 0, records = 0;
    uint64_t seen_min = UINT64_MAX, seen_max = 0;
    uint32_t prev_leaf = SPIKEDB_INVALID_PAGE, last_leaf = SPIKEDB_INVALID_PAGE;
    uint64_t prev_time = 0; uint32_t prev_seq = 0; bool have_prev = false;

    /* A corrupt next_leaf could form a cycle, so bound the walk. */
    uint64_t limit = root.leaf_count + 8;
    uint32_t lp = root.first_leaf;

    while (lp != SPIKEDB_INVALID_PAGE) {
        if (leaves >= limit) {
            vfy_err(r, "symbol %llu: leaf chain exceeds leaf_count %llu (cycle?)",
                    (unsigned long long)symbol, (unsigned long long)root.leaf_count);
            return;
        }
        uint8_t* lb = page_pin(db, lp);
        if (!lb) { vfy_err(r, "symbol %llu: leaf page %u unreadable",
                           (unsigned long long)symbol, lp); return; }
        LeafHeader* h  = (LeafHeader*)lb;

        if (h->symbol != symbol)
            vfy_err(r, "leaf %u: symbol %llu, expected %llu",
                    lp, (unsigned long long)h->symbol, (unsigned long long)symbol);
        if (h->record_size != root.record_size)
            vfy_err(r, "leaf %u: record_size %u, root says %u",
                    lp, h->record_size, root.record_size);
        if (h->prev_leaf != prev_leaf)
            vfy_err(r, "leaf %u: prev_leaf %u, expected %u",
                    lp, h->prev_leaf, prev_leaf);
        if (h->record_count == 0)
            vfy_err(r, "leaf %u: empty leaf left in the chain", lp);

        uint32_t slots_end = LEAF_HDR_SIZE + h->record_count * (uint32_t)LEAF_SLOT_SIZE;
        if (!h->record_size
            && (h->value_heap_bottom > SPDB_PAGE_BODY || slots_end > h->value_heap_bottom))
            vfy_err(r, "leaf %u: slot dir ends at %u, heap bottom %u",
                    lp, slots_end, h->value_heap_bottom);
        if (h->record_size && h->record_count > LEAF_FIXED_CAP(h->record_size))
            vfy_err(r, "leaf %u: %u records exceeds fixed capacity %u",
                    lp, h->record_count, LEAF_FIXED_CAP(h->record_size));

        for (uint32_t i = 0; i < h->record_count; i++) {
            uint64_t kt; uint32_t ks;
            leaf_key_at(lb, i, &kt, &ks);
            if (have_prev && key_cmp(kt, ks, prev_time, prev_seq) <= 0) {
                vfy_err(r, "leaf %u slot %u: key %llu/%u not above %llu/%u",
                        lp, i, (unsigned long long)kt, ks,
                        (unsigned long long)prev_time, prev_seq);
            }
            prev_time = kt; prev_seq = ks; have_prev = true;

            if (!h->record_size) {
                const LeafSlot* ls = (const LeafSlot*)(lb + LEAF_HDR_SIZE);
                if (ls[i].value_offset < h->value_heap_bottom
                    || (uint32_t)ls[i].value_offset + ls[i].value_len > SPDB_PAGE_BODY)
                    vfy_err(r, "leaf %u slot %u: value [%u,+%u) outside the heap",
                            lp, i, ls[i].value_offset, ls[i].value_len);
            }
        }

        if (h->record_count) {
            uint64_t ft, lt; uint32_t fs, lsq;
            leaf_key_at(lb, 0, &ft, &fs);
            leaf_key_at(lb, h->record_count - 1, &lt, &lsq);
            if (h->min_time != ft || h->max_time != lt)
                vfy_err(r, "leaf %u: header range %llu..%llu, records %llu..%llu",
                        lp, (unsigned long long)h->min_time,
                        (unsigned long long)h->max_time,
                        (unsigned long long)ft, (unsigned long long)lt);
            if (ft < seen_min) seen_min = ft;
            if (lt > seen_max) seen_max = lt;
        }

        records += h->record_count;
        leaves++;
        prev_leaf = lp;
        last_leaf = lp;
        uint32_t nx = h->next_leaf;
        page_unpin(db, lp);
        lp = nx;
    }

    if (leaves != root.leaf_count)
        vfy_err(r, "symbol %llu: walked %llu leaves, root says %llu",
                (unsigned long long)symbol, (unsigned long long)leaves,
                (unsigned long long)root.leaf_count);
    if (records != root.record_count)
        vfy_err(r, "symbol %llu: counted %llu records, root says %llu",
                (unsigned long long)symbol, (unsigned long long)records,
                (unsigned long long)root.record_count);
    if (last_leaf != root.last_leaf)
        vfy_err(r, "symbol %llu: chain ends at %u, root says %u",
                (unsigned long long)symbol, last_leaf, root.last_leaf);
    if (records) {
        if (root.min_time != seen_min || root.max_time != seen_max)
            vfy_err(r, "symbol %llu: root range %llu..%llu, leaves %llu..%llu",
                    (unsigned long long)symbol,
                    (unsigned long long)root.min_time,
                    (unsigned long long)root.max_time,
                    (unsigned long long)seen_min, (unsigned long long)seen_max);
    }

    /* Every skip-list level must be ascending, and each node's key must
     * still match the first key of the leaf it points at. */
    for (int lvl = 0; lvl < SPIKEDB_MAX_LEVEL; lvl++) {
        uint64_t ref = root.head_forward[lvl];
        uint64_t steps = 0;
        uint64_t nt = 0; uint32_t ns = 0; bool have = false;
        while (ref != NODE_REF_NIL) {
            if (steps++ > limit) {
                vfy_err(r, "symbol %llu level %d: node chain exceeds %llu (cycle?)",
                        (unsigned long long)symbol, lvl, (unsigned long long)limit);
                break;
            }
            uint32_t np;
            SkipNode* n = node_load(db, ref, &np);
            if (!n) { vfy_err(r, "symbol %llu level %d: node page unreadable",
                              (unsigned long long)symbol, lvl); break; }
            uint64_t ft = n->first_time; uint32_t fs = n->first_seq;
            uint32_t lpg = n->leaf_page;
            uint64_t next = n->forward[lvl];
            page_unpin(db, np);

            if (have && key_cmp(ft, fs, nt, ns) <= 0)
                vfy_err(r, "symbol %llu level %d: node key %llu/%u not above %llu/%u",
                        (unsigned long long)symbol, lvl,
                        (unsigned long long)ft, fs, (unsigned long long)nt, ns);
            nt = ft; ns = fs; have = true;

            if (lpg != SPIKEDB_INVALID_PAGE) {
                uint8_t* lb = page_pin(db, lpg);
                if (!lb) vfy_err(r, "symbol %llu level %d: leaf %u unreadable",
                                 (unsigned long long)symbol, lvl, lpg);
                else {
                    LeafHeader* h = (LeafHeader*)lb;
                    uint64_t kt = 0; uint32_t ks = 0;
                    if (h->record_count) leaf_key_at(lb, 0, &kt, &ks);
                    if (h->symbol != symbol || h->record_count == 0
                        || kt != ft || ks != fs)
                        vfy_err(r, "symbol %llu level %d: node key %llu/%u does not "
                                   "match leaf %u", (unsigned long long)symbol, lvl,
                                (unsigned long long)ft, fs, lpg);
                    page_unpin(db, lpg);
                }
            }
            ref = next;
        }
    }

    r->leaves  += leaves;
    r->records += records;
    r->symbols += 1;
}

SpikeDB_Status spike_db_verify(SpikeDB* db, SpikeDB_VerifyReport* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    memset(out, 0, sizeof(*out));

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_CORRUPT; }

    const MetaPage* m = &db->meta_buf[db->active_meta];
    uint64_t total_pages = m->total_pages_allocated;

    for (uint32_t pg = SPIKEDB_SYMDIR_START;
         pg < SPIKEDB_SYMDIR_START + SPIKEDB_SYMDIR_PAGES; pg++) {
        uint8_t* bytes = page_pin(db, pg);
        if (!bytes) { vfy_err(out, "symbol directory page %u unreadable", pg); continue; }
        SymDirSlot slots[1];
        for (uint32_t i = 0; i < SYMDIR_SLOTS_PER_PAGE; i++) {
            slots[0] = ((const SymDirSlot*)bytes)[i];
            if (!symdir_slot_live(&slots[0])) continue;
            if (slots[0].root_page < SPIKEDB_RESERVED_PAGES
                || slots[0].root_page >= total_pages) {
                vfy_err(out, "symbol %llu: root page %u out of range",
                        (unsigned long long)slots[0].symbol, slots[0].root_page);
                continue;
            }
            page_unpin(db, pg);
            verify_symbol(db, slots[0].symbol, slots[0].root_page, out);
            bytes = page_pin(db, pg);
            if (!bytes) { vfy_err(out, "symbol directory page %u unreadable", pg); break; }
        }
        if (bytes) page_unpin(db, pg);
    }

    /* A corrupt free list would hand the same page out twice, so walk it. */
    uint32_t fl = m->freelist_head;
    uint64_t fl_pages = 0;
    while (fl != SPIKEDB_INVALID_PAGE) {
        if (fl_pages++ > total_pages) {
            vfy_err(out, "free list longer than the file (cycle?)");
            break;
        }
        uint8_t* fb = page_pin(db, fl);
        if (!fb) { vfy_err(out, "free list page %u unreadable", fl); break; }
        FreelistPage* fp = (FreelistPage*)fb;
        if (fp->count > FREELIST_CAPACITY)
            vfy_err(out, "free list page %u: count %u exceeds capacity", fl, fp->count);
        else
            out->free_pages += fp->count;
        uint32_t nx = fp->next_page;
        page_unpin(db, fl);
        fl = nx;
    }
    out->free_pages += fl_pages;   /* the list pages are themselves free space */

    file_unlock(db);
    return out->errors ? SPIKEDB_CORRUPT : SPIKEDB_OK;
}

void spike_db_stats(SpikeDB* db, SpikeDB_Stats* out) {
    if (!db || !out) return;
    memset(out, 0, sizeof(*out));
    MetaPage* m = &db->meta_buf[db->active_meta];
    out->total_pages    = m->total_pages_allocated;
    out->txn_id         = m->txn_id;
    out->cache_capacity = db->cache_capacity;
    out->cache_hits     = db->cache_hits;
    out->cache_misses   = db->cache_misses;
    out->cache_spills   = db->cache_spills;
    out->symbol_count   = m->symbol_count;
    for (uint32_t i = 0; i < db->cache_capacity; i++)
        if (db->slots[i].valid) out->cache_used++;

    /* Best effort: a bad chain reports what it managed to walk rather than
     * failing a diagnostic call. */
    for (uint32_t fl = m->freelist_head; fl != SPIKEDB_INVALID_PAGE; ) {
        if (out->free_pages > out->total_pages) break;
        uint8_t* fb = page_pin(db, fl);
        if (!fb) break;
        FreelistPage* fp = (FreelistPage*)fb;
        uint32_t nx = fp->next_page;
        if (fp->count <= FREELIST_CAPACITY) out->free_pages += fp->count;
        out->free_pages++;              /* the list page is free space too */
        page_unpin(db, fl);
        fl = nx;
    }
}

/*============================================================================
 * Internal structural audit (spike_db_internal.h)
 *
 * spike_db_verify walks the file; this walks what never reaches it. A leaked
 * pin or a stale hash entry is invisible on disk and surfaces much later as
 * an unrelated SPIKEDB_FULL, so it is worth checking at the moment it
 * happens rather than at the moment it hurts.
 *============================================================================*/

int spike_db_internal_check(SpikeDB* db, char* msg, size_t msg_len) {
    if (msg && msg_len) msg[0] = 0;
    if (!db) {
        if (msg && msg_len) snprintf(msg, msg_len, "null handle");
        return 1;
    }

    int  problems = 0;
    char first[192];
    first[0] = 0;

#define AUDIT_ERR(...) do {                                          \
        if (problems == 0) snprintf(first, sizeof(first), __VA_ARGS__); \
        problems++;                                                  \
    } while (0)

    if (db->in_txn) AUDIT_ERR("transaction still in flight");
    if (db->io == NULL) AUDIT_ERR("no I/O table installed");

    uint32_t valid = 0;
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        const CacheSlot* s = &db->slots[i];
        if (!s->valid) {
            if (s->page_id != SPIKEDB_INVALID_PAGE)
                AUDIT_ERR("slot %u: free but still holds page %u", i, s->page_id);
            if (s->pin_count != 0)
                AUDIT_ERR("slot %u: free but pinned %u times", i, s->pin_count);
            continue;
        }
        valid++;
        if (s->page_id == SPIKEDB_INVALID_PAGE)
            AUDIT_ERR("slot %u: valid with no page id", i);
        if (s->pin_count != 0)
            AUDIT_ERR("slot %u: page %u still pinned %u times",
                      i, s->page_id, s->pin_count);
        if (s->dirty)
            AUDIT_ERR("slot %u: page %u dirty outside a transaction", i, s->page_id);
        if (s->fresh)
            AUDIT_ERR("slot %u: page %u marked fresh outside a transaction",
                      i, s->page_id);

        uint32_t h = ht_probe(db, s->page_id);
        if (db->ht[h] == HT_EMPTY)
            AUDIT_ERR("slot %u: page %u missing from the hash table", i, s->page_id);
        else if (db->ht[h] != i)
            AUDIT_ERR("page %u maps to slot %u, not %u", s->page_id, db->ht[h], i);
    }

    uint32_t entries = 0;
    for (uint32_t i = 0; i <= db->ht_mask; i++) {
        if (db->ht[i] == HT_EMPTY) continue;
        entries++;
        uint32_t s = db->ht[i];
        if (s >= db->cache_capacity) {
            AUDIT_ERR("hash entry %u: slot %u out of range", i, s);
            continue;
        }
        if (!db->slots[s].valid)
            AUDIT_ERR("hash entry %u points at free slot %u", i, s);
    }
    if (entries != valid)
        AUDIT_ERR("hash table holds %u entries for %u cached pages", entries, valid);

#undef AUDIT_ERR

    if (msg && msg_len && problems) snprintf(msg, msg_len, "%s", first);
    return problems;
}