/*============================================================================
 * spike_db.c  —  SpikeDB v5 implementation
 *
 * See docs/skiplist-redesign-plan.md for design.
 *
 * Conventions:
 *   - All public symbols prefixed `spike_db_` / `SpikeDB_`.
 *   - Internal statics use `snake_case`.
 *   - All page access goes through page_pin / page_unpin / page_dirty.
 *   - All on-disk structs use #pragma pack(push, 1).
 *============================================================================*/

#include "spike_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  typedef HANDLE spdb_fd_t;
  #define SPDB_INVALID_FD INVALID_HANDLE_VALUE
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  typedef int spdb_fd_t;
  #define SPDB_INVALID_FD (-1)
#endif

/*============================================================================
 * Helpers
 *============================================================================*/

#define SPDB_UNUSED(x) ((void)(x))
#define SPDB_MIN(a,b)  ((a)<(b)?(a):(b))
#define SPDB_MAX(a,b)  ((a)>(b)?(a):(b))

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint32_t crc32_compute(const void* data, size_t len) {
    static uint32_t table[256];
    static int      table_built = 0;
    if (!table_built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1));
            table[i] = c;
        }
        table_built = 1;
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return ~crc;
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
    uint8_t  reserved[SPIKEDB_PAGE_SIZE - 8 - 8 - 8 - 4 - 4 - 8 - 4];
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

#define FREELIST_CAPACITY  ((SPIKEDB_PAGE_SIZE - sizeof(FreelistPage)) / sizeof(uint32_t))

/* Skip-list index node — packed into "node pages" */
typedef struct SkipNode {
    uint64_t first_time;            /* min time of pointed-to leaf */
    uint32_t leaf_page;             /* INVALID for unused slot */
    uint8_t  level;                 /* tower height 1..MAX_LEVEL */
    uint8_t  _pad[3];
    uint64_t forward[SPIKEDB_MAX_LEVEL];   /* node_ref values; 0 = nil */
} SkipNode;

#define NODE_PAGE_HDR_SIZE   16u
#define NODE_PAGE_CAPACITY   ((SPIKEDB_PAGE_SIZE - NODE_PAGE_HDR_SIZE) / sizeof(SkipNode))

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
    uint32_t _pad2;
    uint64_t head_forward[SPIKEDB_MAX_LEVEL];   /* sentinel head's forward refs */
    /* pad to PAGE_SIZE */
} SymbolRootPage;

/* Leaf page header. Slot dir grows up after header; value heap grows down. */
typedef struct LeafHeader {
    uint64_t symbol;
    uint64_t min_time;
    uint64_t max_time;
    uint32_t record_count;
    uint32_t value_heap_bottom;     /* offset of lowest-addressed value byte */
    uint32_t prev_leaf;
    uint32_t next_leaf;
    uint64_t skiplist_node_ref;     /* node_ref of this leaf's index entry */
    uint32_t _pad;
    /* slot dir starts at sizeof(LeafHeader); each slot 16 bytes */
} LeafHeader;

typedef struct LeafSlot {
    uint64_t time;
    uint32_t value_offset;          /* byte offset within page */
    uint16_t value_len;
    uint16_t _pad;
} LeafSlot;

#pragma pack(pop)

#define LEAF_SLOT_SIZE       (sizeof(LeafSlot))
#define LEAF_HDR_SIZE        (sizeof(LeafHeader))

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

    /* meta */
    MetaPage           meta_buf[2];
    int                active_meta;        /* 0 or 1 */
    bool               readonly;

    /* stats */
    uint64_t           cache_hits;
    uint64_t           cache_misses;

    /* current write transaction (single-writer) */
    bool               in_txn;
    uint32_t*          txn_allocated;      /* page ids allocated this txn */
    uint32_t           txn_allocated_count;
    uint32_t           txn_allocated_cap;
    /* On commit, all dirty pages get flushed; meta is then flipped. */
    /* On rollback, allocated pages are returned to the freelist and
       any dirty cache slots are evicted (since their content is the
       half-applied txn). */
};

/*============================================================================
 * I/O helpers
 *============================================================================*/

static SpikeDB_Status io_read(SpikeDB* db, uint32_t page_id, void* buf) {
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
        return SPIKEDB_ERROR;
    }
    if (got < SPIKEDB_PAGE_SIZE)
        memset((uint8_t*)buf + got, 0, SPIKEDB_PAGE_SIZE - got);
#else
    ssize_t n = pread(db->fd, buf, SPIKEDB_PAGE_SIZE, (off_t)off);
    if (n < 0) return SPIKEDB_ERROR;
    if ((size_t)n < SPIKEDB_PAGE_SIZE)
        memset((uint8_t*)buf + n, 0, SPIKEDB_PAGE_SIZE - (size_t)n);
#endif
    return SPIKEDB_OK;
}

static SpikeDB_Status io_write(SpikeDB* db, uint32_t page_id, const void* buf) {
    uint64_t off = (uint64_t)page_id * SPIKEDB_PAGE_SIZE;
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD wrote = 0;
    if (!WriteFile(db->fd, buf, SPIKEDB_PAGE_SIZE, &wrote, &ov))
        return SPIKEDB_ERROR;
    if (wrote != SPIKEDB_PAGE_SIZE) return SPIKEDB_ERROR;
#else
    ssize_t n = pwrite(db->fd, buf, SPIKEDB_PAGE_SIZE, (off_t)off);
    if (n != (ssize_t)SPIKEDB_PAGE_SIZE) return SPIKEDB_ERROR;
#endif
    uint64_t needed = off + SPIKEDB_PAGE_SIZE;
    if (needed > db->file_size) db->file_size = needed;
    return SPIKEDB_OK;
}

static SpikeDB_Status io_fsync(SpikeDB* db) {
#ifdef _WIN32
    return FlushFileBuffers(db->fd) ? SPIKEDB_OK : SPIKEDB_ERROR;
#else
    return fdatasync(db->fd) == 0 ? SPIKEDB_OK : SPIKEDB_ERROR;
#endif
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

static SpikeDB_Status file_lock(SpikeDB* db, bool exclusive) {
#ifdef _WIN32
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(SPDB_LOCK_OFFSET & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(SPDB_LOCK_OFFSET >> 32);
    DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!LockFileEx(db->fd, flags, 0, SPDB_LOCK_LEN, 0, &ov))
        return SPIKEDB_ERROR;
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
    return SPIKEDB_ERROR;
#endif
}

static void file_unlock(SpikeDB* db) {
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
    bool va = (a.magic == SPIKEDB_MAGIC)
            && (crc32_compute(&a, sizeof(MetaPage) - sizeof(uint32_t)) == a.crc32);
    bool vb = (b.magic == SPIKEDB_MAGIC)
            && (crc32_compute(&b, sizeof(MetaPage) - sizeof(uint32_t)) == b.crc32);
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
    for (uint32_t scan = 0; scan < db->cache_capacity * 2u; scan++) {
        uint32_t i = db->clock_hand;
        db->clock_hand = (db->clock_hand + 1) % db->cache_capacity;
        CacheSlot* s = &db->slots[i];
        if (!s->valid) return i;
        if (s->pin_count > 0) continue;
        if (s->dirty) continue;        /* must be flushed first */
        ht_remove(db, s->page_id);
        s->valid   = false;
        s->page_id = SPIKEDB_INVALID_PAGE;
        return i;
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
    if (s == UINT32_MAX) return NULL;
    CacheSlot* slot = &db->slots[s];
    if (io_read(db, page_id, slot->bytes) != SPIKEDB_OK) return NULL;
    slot->page_id   = page_id;
    slot->pin_count = 1;
    slot->dirty     = false;
    slot->valid     = true;
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
        return db->slots[s].bytes;
    }
    uint32_t s = cache_evict(db);
    if (s == UINT32_MAX) return NULL;
    CacheSlot* slot = &db->slots[s];
    memset(slot->bytes, 0, SPIKEDB_PAGE_SIZE);
    slot->page_id   = page_id;
    slot->pin_count = 1;
    slot->dirty     = true;
    slot->valid     = true;
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

static SpikeDB_Status cache_flush_all(SpikeDB* db) {
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        CacheSlot* s = &db->slots[i];
        if (!s->valid || !s->dirty) continue;
        if (io_write(db, s->page_id, s->bytes) != SPIKEDB_OK) return SPIKEDB_ERROR;
        s->dirty = false;
    }
    return SPIKEDB_OK;
}

/* Discard all dirty pages (rollback). */
static void cache_discard_dirty(SpikeDB* db) {
    for (uint32_t i = 0; i < db->cache_capacity; i++) {
        CacheSlot* s = &db->slots[i];
        if (!s->valid || !s->dirty) continue;
        if (s->pin_count != 0) continue;
        ht_remove(db, s->page_id);
        s->valid   = false;
        s->dirty   = false;
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

static SpikeDB_Status txn_commit(SpikeDB* db) {
    if (!db->in_txn) return SPIKEDB_ERROR;
    /* 1. Flush all dirty data pages */
    if (cache_flush_all(db) != SPIKEDB_OK) goto fail;
    /* 2. fsync to make data durable */
    if (io_fsync(db) != SPIKEDB_OK) goto fail;
    /* 3. Bump txn_id, copy active -> inactive, write inactive meta */
    int new_active = 1 - db->active_meta;
    db->meta_buf[new_active] = db->meta_buf[db->active_meta];
    db->meta_buf[new_active].txn_id++;
    db->meta_buf[new_active].reader_epoch++;
    if (meta_write(db, new_active) != SPIKEDB_OK) goto fail;
    /* 4. fsync meta */
    if (io_fsync(db) != SPIKEDB_OK) goto fail;
    /* 5. Flip in-memory active marker */
    db->active_meta = new_active;
    db->in_txn = false;
    db->txn_allocated_count = 0;
    return SPIKEDB_OK;
fail:
    /* Treat as rollback */
    cache_discard_dirty(db);
    db->in_txn = false;
    db->txn_allocated_count = 0;
    return SPIKEDB_ERROR;
}

static void txn_rollback(SpikeDB* db) {
    if (!db->in_txn) return;
    cache_discard_dirty(db);
    /* Allocated pages would have come from freelist or bump; since we
     * discard the dirty meta, the freelist/bump is unchanged in *memory*
     * if we revert the active meta. We work on active meta in place,
     * so we must restore the bump pointer and freelist. To keep this
     * simple, we re-read meta from disk. */
    MetaPage tmp;
    if (io_read(db, db->active_meta == 0 ? SPIKEDB_META_A_PAGE
                                          : SPIKEDB_META_B_PAGE, &tmp) == SPIKEDB_OK) {
        db->meta_buf[db->active_meta] = tmp;
    }
    db->in_txn = false;
    db->txn_allocated_count = 0;
}

/*============================================================================
 * Symbol directory (open-addressing hash, reserved pages)
 *============================================================================*/

#define SYMDIR_TOTAL_SLOTS  (SPIKEDB_SYMDIR_PAGES * (SPIKEDB_PAGE_SIZE / sizeof(SymDirSlot)))

static void symdir_locate(uint64_t symbol, uint32_t* out_page, uint32_t* out_slot) {
    uint64_t h = mix64(symbol);
    uint32_t global = (uint32_t)(h % SYMDIR_TOTAL_SLOTS);
    uint32_t per_pg = SPIKEDB_PAGE_SIZE / (uint32_t)sizeof(SymDirSlot);
    *out_page = SPIKEDB_SYMDIR_START + (global / per_pg);
    *out_slot = global % per_pg;
}

/* Linear-probe to find slot. Returns root_page or INVALID. */
static SpikeDB_Status symdir_lookup(SpikeDB* db, uint64_t symbol,
                                    uint32_t* out_root, bool create_ok) {
    uint32_t pg, slot;
    symdir_locate(symbol, &pg, &slot);
    uint32_t per_pg = SPIKEDB_PAGE_SIZE / (uint32_t)sizeof(SymDirSlot);
    uint64_t probes = 0;

    while (probes < SYMDIR_TOTAL_SLOTS) {
        uint8_t* bytes = page_pin(db, pg);
        if (!bytes) return SPIKEDB_ERROR;
        SymDirSlot* s = (SymDirSlot*)bytes + slot;

        if (s->root_page == 0 && s->symbol == 0) {
            /* Empty */
            if (!create_ok) {
                page_unpin(db, pg);
                return SPIKEDB_NOT_FOUND;
            }
            /* Create new SymbolRoot page */
            uint32_t root_pg;
            if (page_alloc(db, &root_pg) != SPIKEDB_OK) {
                page_unpin(db, pg);
                return SPIKEDB_ERROR;
            }
            uint8_t* rb = page_pin_zero(db, root_pg);
            if (!rb) { page_unpin(db, pg); return SPIKEDB_ERROR; }
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

            s->symbol    = symbol;
            s->root_page = root_pg;
            page_dirty(db, pg);
            page_unpin(db, pg);

            db->meta_buf[db->active_meta].symbol_count++;
            *out_root = root_pg;
            return SPIKEDB_OK;
        }
        if (s->symbol == symbol) {
            uint32_t root = s->root_page;
            page_unpin(db, pg);
            *out_root = root;
            return SPIKEDB_OK;
        }
        page_unpin(db, pg);
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
    h->next_node_page = SPIKEDB_INVALID_PAGE;
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

static SpikeDB_Status descend(SpikeDB* db, uint32_t root_pg, uint64_t time,
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
            page_unpin(db, np);
            if (ft > time) break;
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

/*============================================================================
 * Leaf operations
 *============================================================================*/

static LeafSlot* leaf_slots(uint8_t* page) {
    return (LeafSlot*)(page + LEAF_HDR_SIZE);
}

static uint32_t leaf_free_space(const LeafHeader* h) {
    uint32_t slots_end = LEAF_HDR_SIZE + h->record_count * (uint32_t)LEAF_SLOT_SIZE;
    if (slots_end > h->value_heap_bottom) return 0;
    return h->value_heap_bottom - slots_end;
}

/* Binary search for `time` in a leaf's slot dir. Returns index of first
 * slot whose time >= target, or record_count if none. Sets *exact if found. */
static uint32_t leaf_search(const LeafHeader* h, const LeafSlot* slots,
                            uint64_t time, bool* exact) {
    *exact = false;
    uint32_t lo = 0, hi = h->record_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (slots[mid].time < time) lo = mid + 1;
        else hi = mid;
    }
    if (lo < h->record_count && slots[lo].time == time) *exact = true;
    return lo;
}

/* Initialize a fresh leaf */
static void leaf_init(uint8_t* page, uint64_t symbol) {
    LeafHeader* h = (LeafHeader*)page;
    memset(page, 0, SPIKEDB_PAGE_SIZE);
    h->symbol            = symbol;
    h->min_time          = UINT64_MAX;
    h->max_time          = 0;
    h->record_count      = 0;
    h->value_heap_bottom = SPIKEDB_PAGE_SIZE;
    h->prev_leaf         = SPIKEDB_INVALID_PAGE;
    h->next_leaf         = SPIKEDB_INVALID_PAGE;
    h->skiplist_node_ref = NODE_REF_NIL;
}

/* Try to insert (time, value) into an existing leaf. Returns:
 *   SPIKEDB_OK    — inserted
 *   SPIKEDB_FULL  — caller must split
 *   SPIKEDB_INVAL — duplicate (currently rejected)
 */
static SpikeDB_Status leaf_insert(uint8_t* page, uint64_t time,
                                  const void* value, size_t vlen) {
    LeafHeader* h = (LeafHeader*)page;
    LeafSlot*   slots = leaf_slots(page);

    /* Need room for one slot + vlen bytes */
    uint32_t needed = (uint32_t)(LEAF_SLOT_SIZE + vlen);
    if (leaf_free_space(h) < needed) return SPIKEDB_FULL;

    bool exact;
    uint32_t pos = leaf_search(h, slots, time, &exact);
    if (exact) return SPIKEDB_INVAL;   /* duplicate (symbol,time) — reject */

    /* Place value */
    h->value_heap_bottom -= (uint32_t)vlen;
    memcpy(page + h->value_heap_bottom, value, vlen);

    /* Shift slots[pos..count-1] to [pos+1..count] */
    if (pos < h->record_count) {
        memmove(&slots[pos + 1], &slots[pos],
                (h->record_count - pos) * sizeof(LeafSlot));
    }
    slots[pos].time         = time;
    slots[pos].value_offset = h->value_heap_bottom;
    slots[pos].value_len    = (uint16_t)vlen;
    slots[pos]._pad         = 0;

    h->record_count++;
    if (time < h->min_time) h->min_time = time;
    if (time > h->max_time) h->max_time = time;

    return SPIKEDB_OK;
}

/* Compact a leaf's value heap. Used after a split to reclaim space. */
static void leaf_compact(uint8_t* page) {
    LeafHeader* h = (LeafHeader*)page;
    LeafSlot*   slots = leaf_slots(page);
    /* Copy values to a temp buffer in stack? Pages are 64KB — too big.
     * Compact in place: collect values, write to temp on heap. */
    uint8_t* tmp = (uint8_t*)malloc(SPIKEDB_PAGE_SIZE);
    if (!tmp) return;
    uint32_t cursor = SPIKEDB_PAGE_SIZE;
    for (uint32_t i = 0; i < h->record_count; i++) {
        uint16_t vl = slots[i].value_len;
        cursor -= vl;
        memcpy(tmp + cursor, page + slots[i].value_offset, vl);
        slots[i].value_offset = cursor;
    }
    memcpy(page + cursor, tmp + cursor, SPIKEDB_PAGE_SIZE - cursor);
    h->value_heap_bottom = cursor;
    free(tmp);
}

/*============================================================================
 * Skip-list splice (insert new index node)
 *============================================================================*/

/* Splice a new node with given (level, first_time, leaf_page) using the
 * predecessor refs from a prior descend(). */
static SpikeDB_Status skiplist_splice(SpikeDB* db, uint32_t root_pg,
                                      const DescentResult* desc, int level,
                                      uint64_t first_time, uint32_t leaf_pg,
                                      uint64_t* out_new_ref) {
    uint64_t new_ref;
    if (node_alloc_in_root(db, root_pg, &new_ref) != SPIKEDB_OK) return SPIKEDB_ERROR;

    /* Initialize new node */
    {
        uint32_t np;
        SkipNode* nn = node_load(db, new_ref, &np);
        if (!nn) return SPIKEDB_ERROR;
        nn->first_time = first_time;
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
                                    uint64_t time, const void* value, size_t vlen);

/* Split a leaf into two equal halves around the median, return new leaf id. */
static SpikeDB_Status leaf_split(SpikeDB* db, uint32_t root_pg, uint32_t leaf_pg,
                                 uint32_t* out_new_leaf_pg) {
    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) return SPIKEDB_ERROR;
    LeafHeader* lh = (LeafHeader*)lb;
    LeafSlot*   ls = leaf_slots(lb);
    uint32_t    sym_lo = (uint32_t)lh->symbol;
    SPDB_UNUSED(sym_lo);

    uint32_t mid = lh->record_count / 2;

    /* Allocate new leaf */
    uint32_t new_pg;
    if (page_alloc(db, &new_pg) != SPIKEDB_OK) { page_unpin(db, leaf_pg); return SPIKEDB_ERROR; }
    uint8_t* nb = page_pin_zero(db, new_pg);
    if (!nb) { page_unpin(db, leaf_pg); return SPIKEDB_ERROR; }
    leaf_init(nb, lh->symbol);
    LeafHeader* nh = (LeafHeader*)nb;
    LeafSlot*   ns = leaf_slots(nb);

    /* Move slots [mid..count) to new leaf, copying values */
    for (uint32_t i = mid; i < lh->record_count; i++) {
        uint16_t vl = ls[i].value_len;
        nh->value_heap_bottom -= vl;
        memcpy(nb + nh->value_heap_bottom, lb + ls[i].value_offset, vl);
        ns[i - mid].time         = ls[i].time;
        ns[i - mid].value_offset = nh->value_heap_bottom;
        ns[i - mid].value_len    = vl;
        ns[i - mid]._pad         = 0;
    }
    nh->record_count = lh->record_count - mid;
    nh->min_time     = ns[0].time;
    nh->max_time     = ns[nh->record_count - 1].time;

    /* Truncate old leaf */
    lh->record_count = mid;
    if (mid > 0) {
        lh->min_time = ls[0].time;
        lh->max_time = ls[mid - 1].time;
    } else {
        lh->min_time = UINT64_MAX;
        lh->max_time = 0;
    }
    leaf_compact(lb);

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

    /* Descend with the new leaf's first_time */
    DescentResult d;
    uint64_t new_first;
    {
        uint8_t* nb2 = page_pin(db, new_pg);
        if (!nb2) return SPIKEDB_ERROR;
        new_first = ((LeafHeader*)nb2)->min_time;
        page_unpin(db, new_pg);
    }
    if (descend(db, root_pg, new_first, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint64_t new_ref;
    if (skiplist_splice(db, root_pg, &d, new_level, new_first, new_pg, &new_ref) != SPIKEDB_OK)
        return SPIKEDB_ERROR;

    *out_new_leaf_pg = new_pg;
    return SPIKEDB_OK;
}

static SpikeDB_Status symbol_insert(SpikeDB* db, uint32_t root_pg,
                                    uint64_t time, const void* value, size_t vlen) {
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
            leaf_init(lb, root->symbol);
            SpikeDB_Status st = leaf_insert(lb, time, value, vlen);
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
            return skiplist_splice(db, root_pg, &d, new_level, time, leaf_pg, &new_ref);
        }
        page_unpin(db, root_pg);
    }

    /* General case: descend, find target leaf */
    DescentResult d;
    if (descend(db, root_pg, time, &d) != SPIKEDB_OK) return SPIKEDB_ERROR;
    uint32_t leaf_pg = d.leaf_page;
    if (leaf_pg == SPIKEDB_INVALID_PAGE) {
        /* time < first_leaf.min_time — use first_leaf */
        uint8_t* rb = page_pin(db, root_pg);
        leaf_pg = ((SymbolRootPage*)rb)->first_leaf;
        page_unpin(db, root_pg);
    }

    /* Try insert */
    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) return SPIKEDB_ERROR;
    LeafHeader* lh = (LeafHeader*)lb;

    /* Fast append path: if time > lh->max_time AND this is the last leaf,
     * AND the leaf is full, allocate a new leaf without splitting. */
    if (lh->record_count > 0 && time > lh->max_time
        && leaf_free_space(lh) < (LEAF_SLOT_SIZE + vlen)
        && lh->next_leaf == SPIKEDB_INVALID_PAGE) {
        page_unpin(db, leaf_pg);

        uint32_t new_pg;
        if (page_alloc(db, &new_pg) != SPIKEDB_OK) return SPIKEDB_ERROR;
        uint8_t* nb = page_pin_zero(db, new_pg);
        if (!nb) return SPIKEDB_ERROR;
        leaf_init(nb, lh->symbol);
        SpikeDB_Status st = leaf_insert(nb, time, value, vlen);
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
        if (descend(db, root_pg, time, &d2) != SPIKEDB_OK) return SPIKEDB_ERROR;
        uint64_t new_ref;
        return skiplist_splice(db, root_pg, &d2, new_level, time, new_pg, &new_ref);
    }

    SpikeDB_Status st = leaf_insert(lb, time, value, vlen);
    if (st == SPIKEDB_OK) {
        page_dirty(db, leaf_pg);
        page_unpin(db, leaf_pg);
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
    if (lh2->record_count > 0 && time > lh2->max_time) target = new_pg;
    page_unpin(db, leaf_pg);

    uint8_t* tb = page_pin(db, target);
    if (!tb) return SPIKEDB_ERROR;
    SpikeDB_Status st2 = leaf_insert(tb, time, value, vlen);
    page_dirty(db, target);
    page_unpin(db, target);
    if (st2 != SPIKEDB_OK) return st2;

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
 * Public API: open / close
 *============================================================================*/

static SpikeDB_Status meta_validate(const MetaPage* m) {
    if (m->magic != SPIKEDB_MAGIC) return SPIKEDB_CORRUPT;
    uint32_t want = crc32_compute(m, sizeof(MetaPage) - sizeof(uint32_t));
    if (m->crc32 != want) return SPIKEDB_CORRUPT;
    return SPIKEDB_OK;
}

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
        if (io_write(db, p, zero) != SPIKEDB_OK) { free(zero); return SPIKEDB_ERROR; }
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
    db->readonly = (flags & SPIKEDB_OPEN_READONLY) != 0;

    /* Open file */
#ifdef _WIN32
    DWORD access = db->readonly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE;
    db->fd = CreateFileA(path, access, share, NULL, OPEN_ALWAYS,
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
    return SPIKEDB_OK;

fail:
    spike_db_close(db);
    return SPIKEDB_ERROR;
}

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
    free(db);
}

/*============================================================================
 * Public API: get
 *============================================================================*/

SpikeDB_Status spike_db_get(SpikeDB* db, uint64_t symbol, uint64_t time,
                            void** value_out, size_t* len_out) {
    if (!db || !value_out || !len_out) return SPIKEDB_INVAL;
    *value_out = NULL; *len_out = 0;

    if (file_lock(db, false) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    uint32_t root_pg;
    SpikeDB_Status st = symdir_lookup(db, symbol, &root_pg, false);
    if (st != SPIKEDB_OK) { file_unlock(db); return st; }

    DescentResult d;
    if (descend(db, root_pg, time, &d) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }
    uint32_t leaf_pg = d.leaf_page;
    if (leaf_pg == SPIKEDB_INVALID_PAGE) { file_unlock(db); return SPIKEDB_NOT_FOUND; }

    uint8_t* lb = page_pin(db, leaf_pg);
    if (!lb) { file_unlock(db); return SPIKEDB_ERROR; }
    LeafHeader* lh = (LeafHeader*)lb;
    LeafSlot*   ls = leaf_slots(lb);
    bool exact;
    uint32_t pos = leaf_search(lh, ls, time, &exact);
    if (!exact) {
        page_unpin(db, leaf_pg);
        file_unlock(db);
        return SPIKEDB_NOT_FOUND;
    }
    void* buf = malloc(ls[pos].value_len);
    if (!buf) { page_unpin(db, leaf_pg); file_unlock(db); return SPIKEDB_ERROR; }
    memcpy(buf, lb + ls[pos].value_offset, ls[pos].value_len);
    *value_out = buf;
    *len_out   = ls[pos].value_len;
    page_unpin(db, leaf_pg);
    file_unlock(db);
    return SPIKEDB_OK;
}

void spike_db_free(void* ptr) { free(ptr); }

/*============================================================================
 * Public API: batch
 *============================================================================*/

typedef struct BatchEntry {
    uint64_t symbol;
    uint64_t time;
    uint32_t offset;        /* offset into batch->blob */
    uint32_t length;
} BatchEntry;

struct SpikeDB_Batch {
    BatchEntry* entries;
    size_t      count;
    size_t      cap;
    uint8_t*    blob;
    size_t      blob_size;
    size_t      blob_cap;
};

SpikeDB_Batch* spike_db_batch_create(void) {
    SpikeDB_Batch* b = (SpikeDB_Batch*)calloc(1, sizeof(SpikeDB_Batch));
    return b;
}

void spike_db_batch_destroy(SpikeDB_Batch* b) {
    if (!b) return;
    free(b->entries);
    free(b->blob);
    free(b);
}

void spike_db_batch_clear(SpikeDB_Batch* b) {
    if (!b) return;
    b->count = 0;
    b->blob_size = 0;
}

size_t spike_db_batch_count(const SpikeDB_Batch* b) { return b ? b->count : 0; }

SpikeDB_Status spike_db_batch_put(SpikeDB_Batch* b, uint64_t symbol, uint64_t time,
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
    BatchEntry* e = &b->entries[b->count++];
    e->symbol = symbol;
    e->time   = time;
    e->offset = (uint32_t)b->blob_size;
    e->length = (uint32_t)len;
    if (len) memcpy(b->blob + b->blob_size, value, len);
    b->blob_size += len;
    return SPIKEDB_OK;
}

static int batch_cmp(const void* a, const void* b) {
    const BatchEntry* x = (const BatchEntry*)a;
    const BatchEntry* y = (const BatchEntry*)b;
    if (x->symbol != y->symbol) return x->symbol < y->symbol ? -1 : 1;
    if (x->time   != y->time)   return x->time   < y->time   ? -1 : 1;
    return 0;
}

SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_Batch* b) {
    if (!db || !b) return SPIKEDB_INVAL;
    if (db->readonly) return SPIKEDB_ERROR;
    if (b->count == 0) return SPIKEDB_OK;

    if (file_lock(db, true) != SPIKEDB_OK) return SPIKEDB_ERROR;
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    if (txn_begin(db) != SPIKEDB_OK) { file_unlock(db); return SPIKEDB_ERROR; }

    /* Sort entries by (symbol, time) to amortize symdir lookups */
    qsort(b->entries, b->count, sizeof(BatchEntry), batch_cmp);

    uint64_t cur_sym = b->entries[0].symbol + 1;  /* force lookup on first entry */
    uint32_t root_pg = SPIKEDB_INVALID_PAGE;

    for (size_t i = 0; i < b->count; i++) {
        BatchEntry* e = &b->entries[i];
        if (e->symbol != cur_sym) {
            if (symdir_lookup(db, e->symbol, &root_pg, true) != SPIKEDB_OK) goto rollback;
            cur_sym = e->symbol;
        }
        SpikeDB_Status st = symbol_insert(db, root_pg, e->time, b->blob + e->offset, e->length);
        if (st != SPIKEDB_OK) goto rollback;
    }

    SpikeDB_Status cs = txn_commit(db);
    file_unlock(db);
    return cs;

rollback:
    txn_rollback(db);
    file_unlock(db);
    return SPIKEDB_ERROR;
}

/*============================================================================
 * Public API: scan iterator
 *============================================================================*/

struct SpikeDB_Iter {
    SpikeDB*  db;
    uint64_t  symbol;
    uint64_t  time_lo;
    uint64_t  time_hi;
    uint32_t  cur_leaf;             /* INVALID = done */
    uint32_t  cur_slot;             /* index within cur_leaf */
    uint8_t*  cur_buf;              /* malloc'd buffer holding last value */
    size_t    cur_buf_cap;
    bool      locked;               /* shared lock held? */
};

SpikeDB_Iter* spike_db_scan(SpikeDB* db, uint64_t symbol,
                            uint64_t time_lo, uint64_t time_hi) {
    if (!db || time_hi < time_lo) return NULL;
    SpikeDB_Iter* it = (SpikeDB_Iter*)calloc(1, sizeof(SpikeDB_Iter));
    if (!it) return NULL;
    it->db = db; it->symbol = symbol; it->time_lo = time_lo; it->time_hi = time_hi;
    it->cur_leaf = SPIKEDB_INVALID_PAGE;

    /* Hold a shared lock for the iterator's lifetime so the writer
     * cannot free leaves we're about to walk. */
    if (file_lock(db, false) != SPIKEDB_OK) { free(it); return NULL; }
    if (db_refresh_meta(db) != SPIKEDB_OK) { file_unlock(db); free(it); return NULL; }
    it->locked = true;

    uint32_t root_pg;
    if (symdir_lookup(db, symbol, &root_pg, false) != SPIKEDB_OK) return it;

    DescentResult d;
    if (descend(db, root_pg, time_lo, &d) != SPIKEDB_OK) return it;
    uint32_t lp = d.leaf_page;
    if (lp == SPIKEDB_INVALID_PAGE) {
        uint8_t* rb = page_pin(db, root_pg);
        lp = ((SymbolRootPage*)rb)->first_leaf;
        page_unpin(db, root_pg);
    }
    if (lp == SPIKEDB_INVALID_PAGE) return it;

    /* Find starting slot. May need to advance to next leaf if all times in
     * this leaf are < time_lo. */
    while (lp != SPIKEDB_INVALID_PAGE) {
        uint8_t* lb = page_pin(db, lp);
        if (!lb) return it;
        LeafHeader* lh = (LeafHeader*)lb;
        LeafSlot*   ls = leaf_slots(lb);
        if (lh->record_count > 0 && lh->max_time >= time_lo) {
            bool exact;
            uint32_t pos = leaf_search(lh, ls, time_lo, &exact);
            if (pos < lh->record_count && ls[pos].time <= time_hi) {
                it->cur_leaf = lp;
                it->cur_slot = pos;
                page_unpin(db, lp);
                return it;
            }
        }
        uint32_t nx = lh->next_leaf;
        page_unpin(db, lp);
        lp = nx;
    }
    return it;
}

bool spike_db_iter_next(SpikeDB_Iter* it,
                        uint64_t* time_out,
                        const void** value_out, size_t* len_out) {
    if (!it || it->cur_leaf == SPIKEDB_INVALID_PAGE) return false;

    uint8_t* lb = page_pin(it->db, it->cur_leaf);
    if (!lb) { it->cur_leaf = SPIKEDB_INVALID_PAGE; return false; }
    LeafHeader* lh = (LeafHeader*)lb;
    LeafSlot*   ls = leaf_slots(lb);

    while (it->cur_slot >= lh->record_count
           || ls[it->cur_slot].time > it->time_hi) {
        if (it->cur_slot < lh->record_count && ls[it->cur_slot].time > it->time_hi) {
            page_unpin(it->db, it->cur_leaf);
            it->cur_leaf = SPIKEDB_INVALID_PAGE;
            return false;
        }
        /* Advance to next leaf */
        uint32_t nx = lh->next_leaf;
        page_unpin(it->db, it->cur_leaf);
        it->cur_leaf = nx;
        it->cur_slot = 0;
        if (it->cur_leaf == SPIKEDB_INVALID_PAGE) return false;
        lb = page_pin(it->db, it->cur_leaf);
        if (!lb) { it->cur_leaf = SPIKEDB_INVALID_PAGE; return false; }
        lh = (LeafHeader*)lb;
        ls = leaf_slots(lb);
        if (lh->record_count == 0) continue;
        if (lh->min_time > it->time_hi) {
            page_unpin(it->db, it->cur_leaf);
            it->cur_leaf = SPIKEDB_INVALID_PAGE;
            return false;
        }
    }

    /* Emit current slot. Copy value into iter's buffer so it stays valid
     * across page_unpin. */
    LeafSlot* s = &ls[it->cur_slot];
    if (s->value_len > it->cur_buf_cap) {
        uint8_t* nb = (uint8_t*)realloc(it->cur_buf, s->value_len);
        if (!nb) { page_unpin(it->db, it->cur_leaf); return false; }
        it->cur_buf = nb;
        it->cur_buf_cap = s->value_len;
    }
    memcpy(it->cur_buf, lb + s->value_offset, s->value_len);
    if (time_out)  *time_out  = s->time;
    if (value_out) *value_out = it->cur_buf;
    if (len_out)   *len_out   = s->value_len;
    it->cur_slot++;
    page_unpin(it->db, it->cur_leaf);
    return true;
}

void spike_db_iter_close(SpikeDB_Iter* it) {
    if (!it) return;
    if (it->locked) file_unlock(it->db);
    free(it->cur_buf);
    free(it);
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
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, max_time), out);
    if (st == SPIKEDB_OK && *out == 0) {
        /* check record_count to disambiguate empty */
        uint64_t cnt;
        if (read_root_field(db, symbol, offsetof(SymbolRootPage, record_count), &cnt) == SPIKEDB_OK
            && cnt == 0) return SPIKEDB_NOT_FOUND;
    }
    return st;
}

SpikeDB_Status spike_db_min_time(SpikeDB* db, uint64_t symbol, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, min_time), out);
    if (st == SPIKEDB_OK && *out == UINT64_MAX) return SPIKEDB_NOT_FOUND;
    return st;
}

SpikeDB_Status spike_db_count(SpikeDB* db, uint64_t symbol, uint64_t* out) {
    if (!db || !out) return SPIKEDB_INVAL;
    SpikeDB_Status st = read_root_field(db, symbol, offsetof(SymbolRootPage, record_count), out);
    if (st == SPIKEDB_NOT_FOUND) { *out = 0; return SPIKEDB_OK; }
    return st;
}

/*============================================================================
 * Public API: truncate_before
 *============================================================================*/

/* Remove records with time < cutoff from `leaf`. Updates header. Returns
 * the number of records removed. */
static uint32_t leaf_drop_below(uint8_t* page, uint64_t cutoff) {
    LeafHeader* h = (LeafHeader*)page;
    LeafSlot*   ls = leaf_slots(page);
    if (h->record_count == 0 || h->min_time >= cutoff) return 0;
    bool exact;
    uint32_t cut_idx = leaf_search(h, ls, cutoff, &exact);
    /* cut_idx = first slot with time >= cutoff (NOT exact since exact would
     * mean time == cutoff which we keep). */
    if (cut_idx == 0) return 0;
    uint32_t removed = cut_idx;
    /* Shift slots down */
    memmove(&ls[0], &ls[cut_idx], (h->record_count - cut_idx) * sizeof(LeafSlot));
    h->record_count -= cut_idx;
    if (h->record_count == 0) {
        h->min_time = UINT64_MAX;
        h->max_time = 0;
        h->value_heap_bottom = SPIKEDB_PAGE_SIZE;
    } else {
        h->min_time = ls[0].time;
        h->max_time = ls[h->record_count - 1].time;
        leaf_compact(page);
    }
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
            uint32_t r = leaf_drop_below(lb, cutoff);
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
            if (target_leaf == SPIKEDB_INVALID_PAGE) drop = true;
            else {
                uint8_t* tb = page_pin(db, target_leaf);
                if (!tb) drop = true;
                else {
                    LeafHeader* th = (LeafHeader*)tb;
                    if (th->symbol != symbol || th->record_count == 0
                        || th->max_time < cutoff) drop = true;
                    else new_first_time = th->min_time;
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
                if (nn2 && nn2->first_time != new_first_time) {
                    nn2->first_time = new_first_time;
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
    return cs;

fail:
    txn_rollback(db);
    file_unlock(db);
    return SPIKEDB_ERROR;
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
    out->symbol_count   = m->symbol_count;
    for (uint32_t i = 0; i < db->cache_capacity; i++)
        if (db->slots[i].valid) out->cache_used++;
}
