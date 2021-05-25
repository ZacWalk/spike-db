/*============================================================================
 * spike_db.c  –  SIMD-accelerated, single-file KV store 
 * 
 *   - Double-buffered meta pages with CRC32 checksums
 *   - Reader table in shared mmap (multi-process concurrent reads)
 *   - Named mutex for write serialization across processes
 *   - Flush barriers at commit points
 *
 * Runtime SIMD dispatch: AVX-512 > AVX2 > scalar (chosen via CPUID)
 * Target: Windows (MSVC) / Linux (GCC)
 *============================================================================*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <intrin.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <cpuid.h>
#endif

#include <immintrin.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "spike_db.h"

/*============================================================================
 * Atomic helpers
 *============================================================================*/

#ifdef _WIN32
static inline uint32_t atomic_load32(volatile uint32_t* p)  { return *p; }
static inline uint64_t atomic_load64(volatile uint64_t* p)  { return *p; }
static inline void     atomic_store32(volatile uint32_t* p, uint32_t v) {
    _InterlockedExchange((volatile long*)p, (long)v);
}
static inline void atomic_store64(volatile uint64_t* p, uint64_t v) {
    _InterlockedExchange64((volatile long long*)p, (long long)v);
}
static inline bool cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired) {
    return _InterlockedCompareExchange((volatile long*)p, (long)desired, (long)expected)
           == (long)expected;
}
static inline bool cas64(volatile uint64_t* p, uint64_t expected, uint64_t desired) {
    return _InterlockedCompareExchange64((volatile long long*)p, (long long)desired, (long long)expected)
           == (long long)expected;
}
#else /* POSIX / GCC */
static inline uint32_t atomic_load32(volatile uint32_t* p)  {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline uint64_t atomic_load64(volatile uint64_t* p)  {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store32(volatile uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline void atomic_store64(volatile uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline bool cas32(volatile uint32_t* p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static inline bool cas64(volatile uint64_t* p, uint64_t expected, uint64_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

/*============================================================================
 * CRC32 checksum (hardware-accelerated via SSE4.2)
 *
 * Used to protect Meta Pages against torn writes.  On open, meta pages with
 * invalid checksums are treated as if they don't exist; the other meta page
 * (if valid) becomes the sole active state.
 *============================================================================*/

static uint32_t spikedb_crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, v);
        p += 8;
        len -= 8;
    }
    while (len > 0) {
        crc = _mm_crc32_u8(crc, *p++);
        len--;
    }
    return crc ^ 0xFFFFFFFF;
}

/*============================================================================
 * Page addressing helpers
 *============================================================================*/

static inline void* raw_page_ptr(SpikeDB* db, uint32_t pid) {
    return (uint8_t*)db->mapping + (uint64_t)pid * SPIKEDB_PAGE_SIZE;
}

static inline SpikeDB_Page* page_ptr(SpikeDB* db, uint32_t pid) {
    if (pid == SPIKEDB_INVALID_PAGE) return NULL;
    return (SpikeDB_Page*)raw_page_ptr(db, pid);
}

static inline SpikeDB_DataPageHeader* data_page_hdr(SpikeDB* db, uint32_t pid) {
    return (SpikeDB_DataPageHeader*)raw_page_ptr(db, pid);
}

static inline uint8_t* data_page_payload(SpikeDB* db, uint32_t pid) {
    return (uint8_t*)raw_page_ptr(db, pid) + SPIKEDB_DATA_PAGE_HDR_SIZE;
}

/*============================================================================
 * Random level generator (geometric distribution, p = 0.5)
 *============================================================================*/

static uint16_t random_level(void) {
    static uint64_t s = 0x12345678ABCDEF01ULL;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    uint16_t lvl = 1;
    uint64_t r = s;
    while (lvl < SPIKEDB_MAX_LEVEL && (r & 1)) {
        lvl++;
        r >>= 1;
    }
    return lvl;
}

static uint32_t alloc_page(SpikeDB* db);  /* forward decl */

/*============================================================================
 * Key hash function — wyhash-inspired 64-bit hash
 *============================================================================*/

static inline uint64_t wymix(uint64_t a, uint64_t b) {
#ifdef _MSC_VER
    uint64_t hi;
    uint64_t lo = _umul128(a, b, &hi);
    return hi ^ lo;
#else
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)(r >> 64) ^ (uint64_t)r;
#endif
}

static uint64_t spikedb_hash(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = len ^ 0x9E3779B97F4A7C15ULL;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = wymix(h ^ v, 0xA0761D6478BD642FULL);
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        uint64_t v = 0;
        memcpy(&v, p, len);
        h = wymix(h ^ v, 0xE7037ED1A0B428DBULL);
    }
    return wymix(h, 0x8EBC6AF09C88C6E3ULL);
}

/*============================================================================
 * Data-offset encoding helpers
 *============================================================================*/

static inline uint64_t data_offset_encode(uint32_t page_id, uint32_t byte_off) {
    return ((uint64_t)page_id << 32) | (uint64_t)byte_off;
}
static inline uint32_t data_offset_page(uint64_t doff) { return (uint32_t)(doff >> 32); }
static inline uint32_t data_offset_byte(uint64_t doff) { return (uint32_t)(doff & 0xFFFFFFFF); }

/*============================================================================
 * Data record helpers
 *============================================================================*/

static inline const uint8_t* data_record_ptr(SpikeDB* db, uint64_t doff) {
    return data_page_payload(db, data_offset_page(doff)) + data_offset_byte(doff);
}

static bool data_record_key_matches(SpikeDB* db, uint64_t doff,
                                     const char* key, size_t keylen) {
    const uint8_t* rec = data_record_ptr(db, doff);
    uint32_t stored_klen;
    memcpy(&stored_klen, rec, 4);
    if (stored_klen != (uint32_t)keylen) return false;
    return memcmp(rec + 8, key, keylen) == 0;
}

static char* data_record_read_value(SpikeDB* db, uint64_t doff, size_t* vallen_out) {
    const uint8_t* rec = data_record_ptr(db, doff);
    uint32_t kl, vl;
    memcpy(&kl, rec, 4);
    memcpy(&vl, rec + 4, 4);
    char* val = (char*)malloc(vl > 0 ? vl : 1);
    if (!val) return NULL;
    if (vl > 0) memcpy(val, rec + 8 + kl, vl);
    *vallen_out = vl;
    return val;
}

static uint64_t alloc_data_record(SpikeDB* db, const char* key, size_t keylen,
                                   const char* val, size_t vallen) {
    uint32_t record_size = 8 + (uint32_t)keylen + (uint32_t)vallen;
    uint32_t dpid = db->meta[db->active_meta]->data_page_head;

    if (dpid == SPIKEDB_INVALID_PAGE ||
        data_page_hdr(db, dpid)->used_bytes + record_size > SPIKEDB_DATA_PAGE_PAYLOAD) {
        uint32_t new_dpid = alloc_page(db);
        if (new_dpid == SPIKEDB_INVALID_PAGE) return UINT64_MAX;
        memset(raw_page_ptr(db, new_dpid), 0, SPIKEDB_PAGE_SIZE);
        db->meta[db->active_meta]->data_page_head = new_dpid;
        dpid = new_dpid;
    }

    SpikeDB_DataPageHeader* hdr = data_page_hdr(db, dpid);
    uint32_t offset = hdr->used_bytes;
    uint8_t* dest = data_page_payload(db, dpid) + offset;

    uint32_t kl = (uint32_t)keylen;
    uint32_t vl = (uint32_t)vallen;
    memcpy(dest, &kl, 4);
    memcpy(dest + 4, &vl, 4);
    if (keylen > 0) memcpy(dest + 8, key, keylen);
    if (vallen > 0) memcpy(dest + 8 + keylen, val, vallen);

    hdr->used_bytes += record_size;
    return data_offset_encode(dpid, offset);
}

/*============================================================================
 * Bloom filter
 *============================================================================*/

static inline void bloom_hash(uint64_t key, uint32_t positions[8]) {
    static const uint64_t primes[8] = {
        0x9E3779B97F4A7C15ULL, 0x517CC1B727220A95ULL,
        0x6C62272E07BB0142ULL, 0xBF58476D1CE4E5B9ULL,
        0x94D049BB133111EBULL, 0xD6E8FEB86659FD93ULL,
        0xA0761D6478BD642FULL, 0xE7037ED1A0B428DBULL
    };
    for (int i = 0; i < 8; i++) {
        uint64_t h = key * primes[i];
        h ^= h >> 33;
        positions[i] = (uint32_t)((h >> 6) & 7) * 64 + (uint32_t)(h & 63);
    }
}

static void bloom_add(SpikeDB_BloomBlock* blk, uint64_t key) {
    uint32_t pos[8];
    bloom_hash(key, pos);
    for (int i = 0; i < 8; i++) {
        uint32_t word = pos[i] / 64;
        uint32_t bit  = pos[i] % 64;
        blk->words[word] |= (1ULL << bit);
    }
}

/*============================================================================
 * ── SCALAR implementations ──
 *============================================================================*/

static bool bloom_check_scalar(const SpikeDB_BloomBlock* blk, uint64_t key) {
    uint32_t pos[8];
    bloom_hash(key, pos);
    for (int i = 0; i < 8; i++) {
        uint32_t word = pos[i] / 64;
        uint32_t bit  = pos[i] % 64;
        if (!(blk->words[word] & (1ULL << bit)))
            return false;
    }
    return true;
}

static int find_key_scalar(const uint64_t* keys, uint16_t count, uint64_t target) {
    int lo = 0, hi = (int)count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    /* lo is now the first position where keys[lo] >= target.
     * Return it if it's an exact match. */
    if (lo < (int)count && keys[lo] == target) return lo;
    return -1;
}

static int find_exit_scalar(const uint64_t* keys, uint16_t count, uint64_t target) {
    if (count == 0) return -1;
    int lo = 0, hi = (int)count - 1;
    int result = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (keys[mid] <= target) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

/*============================================================================
 * ── AVX2 implementations ──
 *============================================================================*/

static bool bloom_check_avx2(const SpikeDB_BloomBlock* blk, uint64_t key) {
    uint32_t pos[8];
    bloom_hash(key, pos);
#ifdef _MSC_VER
    __declspec(align(32)) uint64_t mask_arr[8] = {0};
#else
    uint64_t mask_arr[8] __attribute__((aligned(32))) = {0};
#endif
    for (int i = 0; i < 8; i++) {
        uint32_t word = pos[i] / 64;
        uint32_t bit  = pos[i] % 64;
        mask_arr[word] |= (1ULL << bit);
    }
    __m256i f0 = _mm256_load_si256((const __m256i*)&blk->words[0]);
    __m256i m0 = _mm256_load_si256((const __m256i*)&mask_arr[0]);
    __m256i a0 = _mm256_and_si256(f0, m0);
    __m256i x0 = _mm256_xor_si256(a0, m0);
    __m256i f1 = _mm256_load_si256((const __m256i*)&blk->words[4]);
    __m256i m1 = _mm256_load_si256((const __m256i*)&mask_arr[4]);
    __m256i a1 = _mm256_and_si256(f1, m1);
    __m256i x1 = _mm256_xor_si256(a1, m1);
    __m256i combined = _mm256_or_si256(x0, x1);
    return _mm256_testz_si256(combined, combined) != 0;
}

static int find_key_avx2(const uint64_t* keys, uint16_t count, uint64_t target) {
    __m256i vtarget = _mm256_set1_epi64x((long long)target);
    int i = 0;
    for (; i + 4 <= (int)count; i += 4) {
        __m256i vkeys = _mm256_loadu_si256((const __m256i*)(keys + i));
        __m256i cmp   = _mm256_cmpeq_epi64(vkeys, vtarget);
        int mask      = _mm256_movemask_epi8(cmp);
        if (mask) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanForward(&idx, (unsigned long)mask);
#else
            int idx = __builtin_ctz((unsigned int)mask);
#endif
            return i + (int)(idx / 8);
        }
    }
    for (; i < (int)count; i++)
        if (keys[i] == target) return i;
    return -1;
}

static int find_exit_avx2(const uint64_t* keys, uint16_t count, uint64_t target) {
    int exit_idx = -1;
    __m256i vtarget = _mm256_set1_epi64x((long long)target);
    int i = 0;
    for (; i + 4 <= (int)count; i += 4) {
        __m256i vkeys = _mm256_loadu_si256((const __m256i*)(keys + i));
        __m256i sign  = _mm256_set1_epi64x((long long)0x8000000000000000ULL);
        __m256i sk    = _mm256_xor_si256(vkeys, sign);
        __m256i st    = _mm256_xor_si256(vtarget, sign);
        __m256i gt    = _mm256_cmpgt_epi64(sk, st);
        int mask      = _mm256_movemask_epi8(gt);
        int le_mask   = (~mask) & 0xFFFFFFFF;
        for (int j = 3; j >= 0; j--) {
            if (le_mask & (0xFF << (j * 8))) {
                int candidate = i + j;
                if (candidate < (int)count && candidate > exit_idx)
                    exit_idx = candidate;
                break;
            }
        }
    }
    for (; i < (int)count; i++)
        if (keys[i] <= target && i > exit_idx) exit_idx = i;
    return exit_idx;
}

/*============================================================================
 * ── AVX-512 implementations ──
 *============================================================================*/

#if defined(__AVX512F__) || defined(_SPIKEDB_COMPILE_AVX512)

static bool bloom_check_avx512(const SpikeDB_BloomBlock* blk, uint64_t key) {
    uint32_t pos[8];
    bloom_hash(key, pos);
#ifdef _MSC_VER
    __declspec(align(64)) uint64_t mask_arr[8] = {0};
#else
    uint64_t mask_arr[8] __attribute__((aligned(64))) = {0};
#endif
    for (int i = 0; i < 8; i++) {
        uint32_t word = pos[i] / 64;
        uint32_t bit  = pos[i] % 64;
        mask_arr[word] |= (1ULL << bit);
    }
    __m512i filter = _mm512_load_si512((const __m512i*)blk);
    __m512i mask   = _mm512_load_si512((const __m512i*)mask_arr);
    __m512i anded  = _mm512_and_si512(filter, mask);
    __mmask8 eq    = _mm512_cmpeq_epi64_mask(anded, mask);
    return eq == 0xFF;
}

static int find_key_avx512(const uint64_t* keys, uint16_t count, uint64_t target) {
    __m512i vtarget = _mm512_set1_epi64((long long)target);
    for (int i = 0; i < (int)count; i += 8) {
        int remaining = count - i;
        if (remaining >= 8) {
            __m512i vkeys = _mm512_loadu_si512((const __m512i*)(keys + i));
            __mmask8 m    = _mm512_cmpeq_epi64_mask(vkeys, vtarget);
            if (m) {
#ifdef _MSC_VER
                unsigned long idx;
                _BitScanForward(&idx, (unsigned long)m);
#else
                int idx = __builtin_ctz((unsigned int)m);
#endif
                return i + (int)idx;
            }
        } else {
            __mmask8 tail = (__mmask8)((1u << remaining) - 1);
            __m512i vkeys = _mm512_maskz_loadu_epi64(tail, keys + i);
            __mmask8 m    = _mm512_mask_cmpeq_epi64_mask(tail, vkeys, vtarget);
            if (m) {
#ifdef _MSC_VER
                unsigned long idx;
                _BitScanForward(&idx, (unsigned long)m);
#else
                int idx = __builtin_ctz((unsigned int)m);
#endif
                return i + (int)idx;
            }
        }
    }
    return -1;
}

static int find_exit_avx512(const uint64_t* keys, uint16_t count, uint64_t target) {
    int exit_idx = -1;
    __m512i vtarget = _mm512_set1_epi64((long long)target);
    for (int i = 0; i < (int)count; i += 8) {
        int remaining = count - i;
        __mmask8 load_mask = (remaining >= 8) ? 0xFF : (__mmask8)((1u << remaining) - 1);
        __m512i vkeys = _mm512_maskz_loadu_epi64(load_mask, keys + i);
        __mmask8 le   = _mm512_mask_cmple_epu64_mask(load_mask, vkeys, vtarget);
        if (le) {
#ifdef _MSC_VER
            unsigned long idx;
            _BitScanReverse(&idx, (unsigned long)le);
#else
            int idx = 31 - __builtin_clz((unsigned int)le);
#endif
            int candidate = i + (int)idx;
            if (candidate > exit_idx) exit_idx = candidate;
        }
    }
    return exit_idx;
}

#endif /* AVX-512 */

/*============================================================================
 * CPUID-based runtime detection & dispatch
 *============================================================================*/

#ifdef _WIN32
static SpikeDB_SimdLevel detect_simd(void) {
    int info[4] = {0};
    __cpuidex(info, 7, 0);
    bool has_avx2    = (info[1] & (1 << 5))  != 0;
    bool has_avx512f = (info[1] & (1 << 16)) != 0;
    __cpuid(info, 1);
    bool os_xsave = (info[2] & (1 << 27)) != 0;
    if (!os_xsave) return SPIKEDB_SIMD_SCALAR;
    uint64_t xcr0 = _xgetbv(0);
    bool ymm_ok = (xcr0 & 0x06) == 0x06;
    bool zmm_ok = (xcr0 & 0xE6) == 0xE6;
    if (has_avx512f && zmm_ok) return SPIKEDB_SIMD_AVX512;
    if (has_avx2    && ymm_ok) return SPIKEDB_SIMD_AVX2;
    return SPIKEDB_SIMD_SCALAR;
}
#else /* POSIX / GCC */
static inline uint64_t spikedb_xgetbv(unsigned int index) {
    unsigned int eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t)edx << 32) | eax;
}
static SpikeDB_SimdLevel detect_simd(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return SPIKEDB_SIMD_SCALAR;
    bool has_avx2    = (ebx & (1 << 5))  != 0;
    bool has_avx512f = (ebx & (1 << 16)) != 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return SPIKEDB_SIMD_SCALAR;
    bool os_xsave = (ecx & (1 << 27)) != 0;
    if (!os_xsave) return SPIKEDB_SIMD_SCALAR;
    uint64_t xcr0 = spikedb_xgetbv(0);
    bool ymm_ok = (xcr0 & 0x06) == 0x06;
    bool zmm_ok = (xcr0 & 0xE6) == 0xE6;
    if (has_avx512f && zmm_ok) return SPIKEDB_SIMD_AVX512;
    if (has_avx2    && ymm_ok) return SPIKEDB_SIMD_AVX2;
    return SPIKEDB_SIMD_SCALAR;
}
#endif

static void init_simd_ops(SpikeDB* db) {
    db->simd_level = detect_simd();
    switch (db->simd_level) {
#if defined(__AVX512F__) || defined(_SPIKEDB_COMPILE_AVX512)
    case SPIKEDB_SIMD_AVX512:
        db->ops.bloom_check = bloom_check_avx512;
        db->ops.find_key    = find_key_avx512;
        db->ops.find_exit   = find_exit_avx512;
        break;
#endif
    case SPIKEDB_SIMD_AVX2:
        db->ops.bloom_check = bloom_check_avx2;
        db->ops.find_key    = find_key_avx2;
        db->ops.find_exit   = find_exit_avx2;
        break;
    default:
        db->ops.bloom_check = bloom_check_scalar;
        db->ops.find_key    = find_key_scalar;
        db->ops.find_exit   = find_exit_scalar;
        break;
    }
}

SpikeDB_SimdLevel spike_db_simd_level(SpikeDB* db) {
    return db->simd_level;
}

/*============================================================================
 * Page Allocation / Freelist
 *
 * The writer holds the mutex, so these are NOT lock-free — they
 * simply read/write the active meta page's freelist and bump allocator.
 *============================================================================*/

static uint32_t alloc_page(SpikeDB* db) {
    SpikeDB_MetaPage* m = db->meta[db->active_meta];
    /* Try freelist */
    if (m->freelist_head != SPIKEDB_INVALID_PAGE) {
        uint32_t head = m->freelist_head;
        SpikeDB_Page* p = page_ptr(db, head);
        m->freelist_head = p->next_page_ids[0];
        return head;
    }
    /* Bump allocator */
    uint64_t max_pages = db->mapping_size / SPIKEDB_PAGE_SIZE;
    if (m->total_pages_allocated >= max_pages) return SPIKEDB_INVALID_PAGE;
    uint32_t pid = (uint32_t)m->total_pages_allocated;
    m->total_pages_allocated++;
    return pid;
}

static void free_page_deferred(SpikeDB* db, uint32_t pid) {
    SpikeDB_MetaPage* m = db->meta[db->active_meta];
    SpikeDB_Page* p = page_ptr(db, pid);
    p->next_page_ids[0] = m->freelist_head;
    m->freelist_head = pid;
}

/*============================================================================
 * Meta Page management (double-buffered, CRC32-protected)
 *
 * On commit, we write the new state to the OLDER meta page (the one NOT
 * currently active).  This ensures that the currently-active meta page is
 * never partially written.  If the process crashes mid-write, the active
 * meta page still holds the last committed state.
 *============================================================================*/

/* Compute CRC32 over the meaningful fields (bytes 0..35, before the checksum) */
static uint32_t meta_compute_checksum(const SpikeDB_MetaPage* m) {
    return spikedb_crc32(m, 36);  /* magic(8) + txn_id(8) + root(4) + free(4) + alloc(8) + data(4) = 36 */
}

/* Validate a meta page: magic matches and checksum is correct */
static bool meta_is_valid(const SpikeDB_MetaPage* m) {
    if (m->magic != SPIKEDB_MAGIC) return false;
    return m->checksum == meta_compute_checksum(m);
}

/* Write the current active meta state to the INACTIVE meta page, advancing
 * txn_id, then flip active_meta.
 *
 * Durability model:
 *   - All writes go through the mmap and are visible to other processes
 *     immediately via shared mapping.
 *   - The OS lazily flushes dirty pages to disk.  On process crash (but OS
 *     survives), committed data is safe in the kernel page cache.
 *   - spike_db_close() issues a full FlushViewOfFile + FlushFileBuffers for
 *     guaranteed on-disk durability.
 *   - On OS/power crash, the database recovers from whichever meta page has
 *     the higher valid (CRC32-checked) txn_id.  At most the last few
 *     committed transactions may be lost, but the DB is never corrupt.
 */
static void meta_commit(SpikeDB* db) {
    int old_active = db->active_meta;
    int new_slot   = 1 - old_active;
    SpikeDB_MetaPage* src = db->meta[old_active];
    SpikeDB_MetaPage* dst = db->meta[new_slot];

    /* Write new meta: copy current state, bump txn_id, recompute checksum */
    dst->magic                = src->magic;
    dst->txn_id               = src->txn_id + 1;
    dst->root_page_id         = src->root_page_id;
    dst->freelist_head        = src->freelist_head;
    dst->total_pages_allocated = src->total_pages_allocated;
    dst->data_page_head       = src->data_page_head;
    dst->checksum             = meta_compute_checksum(dst);

    /* Memory barrier: ensure meta page fields are committed to cache
     * before we flip active_meta (visible to readers in same process). */
#ifdef _MSC_VER
    _WriteBarrier();
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif

    /* Update in-memory active pointer */
    db->active_meta = new_slot;
    db->cur_meta = db->meta[new_slot];
}

/*============================================================================
 * Reader Table management (multi-process, in shared mmap)
 *
 * Each reader (spike_db_get) acquires a slot, sets it to the current txn_id,
 * performs the read, then releases the slot.  The writer checks all slots to
 * determine the minimum active txn_id for safe page reclamation.
 *============================================================================*/

/* Clear slots belonging to dead processes (stale reader detection) */
static void reader_table_clear_stale(SpikeDB* db) {
    for (uint32_t i = 0; i < db->reader_hdr->max_readers; i++) {
        SpikeDB_ReaderSlot* slot = &db->reader_slots[i];
        uint32_t pid = slot->pid;
        if (pid == 0 || slot->txn_id == 0) continue;

        /* Check if the process is still alive */
#ifdef _WIN32
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc == NULL) {
            /* Process is dead — clear the stale slot */
            slot->txn_id = 0;
            slot->pid    = 0;
        } else {
            CloseHandle(hProc);
        }
#else
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
            slot->txn_id = 0;
            slot->pid    = 0;
        }
#endif
    }
}

/* Acquire a reader slot and record the current txn_id.
 * Returns the slot index, or -1 if the table is full. */
static int reader_acquire(SpikeDB* db) {
#ifdef _WIN32
    uint32_t my_pid = GetCurrentProcessId();
#else
    uint32_t my_pid = (uint32_t)getpid();
#endif
    uint64_t cur_txn = db->meta[db->active_meta]->txn_id;

    for (uint32_t i = 0; i < db->reader_hdr->max_readers; i++) {
        SpikeDB_ReaderSlot* slot = &db->reader_slots[i];
        if (slot->txn_id == 0) {
            uint64_t expected = 0;
            if (cas64(&slot->txn_id, expected, cur_txn)) {
                slot->pid = my_pid;
                return (int)i;
            }
        }
    }
    return -1;
}

/* Release a reader slot */
static void reader_release(SpikeDB* db, int slot_idx) {
    if (slot_idx < 0) return;
    SpikeDB_ReaderSlot* slot = &db->reader_slots[slot_idx];
    slot->pid    = 0;
#ifdef _MSC_VER
    _ReadWriteBarrier();
#else
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
#endif
    slot->txn_id = 0;
}

/*============================================================================
 * Write Mutex helpers (cross-process serialization)
 *============================================================================*/

#ifdef _WIN32
/* Derive a deterministic mutex name from the file path */
static HANDLE create_write_mutex(const char* path) {
    char full_path[MAX_PATH];
    DWORD len = GetFullPathNameA(path, MAX_PATH, full_path, NULL);
    if (len == 0) return NULL;

    /* FNV-1a hash of the normalised path */
    uint32_t h = 0x811C9DC5u;
    for (DWORD i = 0; i < len; i++) {
        char c = full_path[i];
        if (c >= 'A' && c <= 'Z') c += 32;  /* lowercase */
        if (c == '/') c = '\\';
        h ^= (uint32_t)(unsigned char)c;
        h *= 0x01000193u;
    }

    char mutex_name[64];
    sprintf_s(mutex_name, sizeof(mutex_name), "SpikeDB_WMtx_%08X", h);
    return CreateMutexA(NULL, FALSE, mutex_name);
}

static void write_lock(SpikeDB* db) {
    WaitForSingleObject((HANDLE)db->write_mutex, INFINITE);
}

static void write_unlock(SpikeDB* db) {
    ReleaseMutex((HANDLE)db->write_mutex);
}
#else /* POSIX */
static int create_lock_fd(const char* path) {
    char lock_path[PATH_MAX];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    return fd;
}

static void write_lock(SpikeDB* db) {
    flock((int)(intptr_t)db->write_mutex, LOCK_EX);
}

static void write_unlock(SpikeDB* db) {
    flock((int)(intptr_t)db->write_mutex, LOCK_UN);
}
#endif

/*============================================================================
 * Database Open / Close
 *============================================================================*/

SpikeDB_Status spike_db_open(SpikeDB** out, const char* path, uint32_t max_size_gb,
                             uint32_t flags) {
    *out = NULL;

    uint64_t file_size = (uint64_t)max_size_gb * 1024ULL * 1024ULL * 1024ULL;
    if (file_size < (uint64_t)SPIKEDB_PAGE_SIZE * (SPIKEDB_RESERVED_PAGES + 2))
        file_size = (uint64_t)SPIKEDB_PAGE_SIZE * (SPIKEDB_RESERVED_PAGES + 2);

    SpikeDB* db = (SpikeDB*)calloc(1, sizeof(SpikeDB));
    if (!db) return SPIKEDB_ERROR;

    db->exclusive = (flags & SPIKEDB_OPEN_EXCLUSIVE) != 0;

#ifdef _WIN32
    HANDLE hMutex = NULL;
    if (!db->exclusive) {
        /* Create the named write mutex BEFORE opening the file, so that
         * initialization is serialized across processes. */
        hMutex = create_write_mutex(path);
        if (!hMutex) { free(db); return SPIKEDB_ERROR; }
        db->write_mutex = hMutex;
    }

    HANDLE hFile = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        db->exclusive ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE),
        NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        if (hMutex) CloseHandle(hMutex);
        free(db); return SPIKEDB_ERROR;
    }

    /* Acquire write mutex during initialization to prevent races */
    if (hMutex) WaitForSingleObject(hMutex, INFINITE);

    DWORD dummy;
    DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dummy, NULL);

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)file_size;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) || !SetEndOfFile(hFile)) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        CloseHandle(hFile); free(db);
        return SPIKEDB_ERROR;
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READWRITE,
                                     (DWORD)(file_size >> 32), (DWORD)file_size, NULL);
    if (!hMap) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        CloseHandle(hFile); free(db);
        return SPIKEDB_ERROR;
    }

    void* base = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        CloseHandle(hMap); CloseHandle(hFile); free(db);
        return SPIKEDB_ERROR;
    }

    db->mapping      = base;
    db->mapping_size = file_size;
    db->file_handle  = hFile;
    db->map_handle   = hMap;
#else /* POSIX */
    int lock_fd = -1;
    if (!db->exclusive) {
        lock_fd = create_lock_fd(path);
        if (lock_fd < 0) { free(db); return SPIKEDB_ERROR; }
        db->write_mutex = (void*)(intptr_t)lock_fd;
    }

    int fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        if (lock_fd >= 0) close(lock_fd);
        free(db); return SPIKEDB_ERROR;
    }

    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);

    if (ftruncate(fd, (off_t)file_size) != 0) {
        if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
        close(fd); free(db);
        return SPIKEDB_ERROR;
    }

    void* base = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
        close(fd); free(db);
        return SPIKEDB_ERROR;
    }

    db->mapping      = base;
    db->mapping_size = file_size;
    db->file_handle  = (void*)(intptr_t)fd;
    db->map_handle   = NULL;
#endif

    /* Set up pointers to the meta pages and reader table */
    db->meta[0]      = (SpikeDB_MetaPage*)raw_page_ptr(db, 0);
    db->meta[1]      = (SpikeDB_MetaPage*)raw_page_ptr(db, 1);
    db->reader_hdr   = (SpikeDB_ReaderTableHeader*)raw_page_ptr(db, 2);
    db->reader_slots = (SpikeDB_ReaderSlot*)((uint8_t*)db->reader_hdr + sizeof(SpikeDB_ReaderTableHeader));

    init_simd_ops(db);

    /* ---- Determine database state from meta pages ---- */
    bool m0_valid = meta_is_valid(db->meta[0]);
    bool m1_valid = meta_is_valid(db->meta[1]);

    if (!m0_valid && !m1_valid) {
        /* Brand-new database — initialize both meta pages and reader table */
        memset(raw_page_ptr(db, 0), 0, SPIKEDB_PAGE_SIZE * SPIKEDB_RESERVED_PAGES);

        db->meta[0]->magic                = SPIKEDB_MAGIC;
        db->meta[0]->txn_id               = 1;
        db->meta[0]->root_page_id         = SPIKEDB_INVALID_PAGE;
        db->meta[0]->freelist_head        = SPIKEDB_INVALID_PAGE;
        db->meta[0]->total_pages_allocated = SPIKEDB_RESERVED_PAGES;
        db->meta[0]->data_page_head       = SPIKEDB_INVALID_PAGE;
        db->meta[0]->checksum             = meta_compute_checksum(db->meta[0]);

        memcpy(db->meta[1], db->meta[0], sizeof(SpikeDB_MetaPage));
        db->meta[1]->txn_id   = 0;
        db->meta[1]->checksum = meta_compute_checksum(db->meta[1]);

        db->reader_hdr->magic       = SPIKEDB_READER_TABLE_MAGIC;
        db->reader_hdr->max_readers = SPIKEDB_MAX_READERS;

#ifdef _WIN32
        FlushViewOfFile(base, SPIKEDB_PAGE_SIZE * SPIKEDB_RESERVED_PAGES);
        FlushFileBuffers(hFile);
#else
        msync(base, SPIKEDB_PAGE_SIZE * SPIKEDB_RESERVED_PAGES, MS_SYNC);
        fdatasync(fd);
#endif

        db->active_meta = 0;
    } else if (m0_valid && m1_valid) {
        db->active_meta = (db->meta[1]->txn_id > db->meta[0]->txn_id) ? 1 : 0;
    } else {
        db->active_meta = m0_valid ? 0 : 1;
    }

    db->cur_meta = db->meta[db->active_meta];

    /* Clear stale reader slots from crashed processes */
    if (!db->exclusive)
        reader_table_clear_stale(db);

#ifdef _WIN32
    if (hMutex) ReleaseMutex(hMutex);
#else
    if (lock_fd >= 0) flock(lock_fd, LOCK_UN);
#endif

    *out = db;
    return SPIKEDB_OK;
}

void spike_db_close(SpikeDB* db) {
    if (!db) return;
#ifdef _WIN32
    if (db->mapping) {
        FlushViewOfFile(db->mapping, 0);
        FlushFileBuffers((HANDLE)db->file_handle);
        UnmapViewOfFile(db->mapping);
    }
    if (db->map_handle)  CloseHandle((HANDLE)db->map_handle);
    if (db->file_handle) CloseHandle((HANDLE)db->file_handle);
    if (db->write_mutex) CloseHandle((HANDLE)db->write_mutex);
#else
    if (db->mapping) {
        msync(db->mapping, (size_t)db->mapping_size, MS_SYNC);
        int fd = (int)(intptr_t)db->file_handle;
        if (fd >= 0) fdatasync(fd);
        munmap(db->mapping, (size_t)db->mapping_size);
    }
    {
        int fd = (int)(intptr_t)db->file_handle;
        if (fd >= 0) close(fd);
    }
    {
        int lfd = (int)(intptr_t)db->write_mutex;
        if (lfd >= 0) close(lfd);
    }
#endif
    free(db);
}

/*============================================================================
 * Page helpers
 *============================================================================*/

static void page_init(SpikeDB_Page* p, uint16_t level) {
    memset(p, 0, SPIKEDB_PAGE_SIZE);
    p->level = level;
    for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++)
        p->next_page_ids[i] = SPIKEDB_INVALID_PAGE;
}

static int page_find_verified(SpikeDB* db, SpikeDB_Page* p, uint64_t hash,
                               const char* key, size_t keylen) {
    int start = 0;
    while (start < p->key_count) {
        int idx = db->ops.find_key(p->hashes + start, (uint16_t)(p->key_count - start), hash);
        if (idx < 0) return -1;
        idx += start;
        if (data_record_key_matches(db, p->data_offsets[idx], key, keylen))
            return idx;
        start = idx + 1;
    }
    return -1;
}

static bool page_insert_entry(SpikeDB* db, SpikeDB_Page* p, uint64_t hash,
                               uint64_t data_off, const char* key, size_t keylen) {
    if (p->key_count >= SPIKEDB_KEYS_PER_PAGE) return false;

    int lo = 0, hi = p->key_count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (p->hashes[mid] < hash) lo = mid + 1;
        else hi = mid;
    }
    int pos = lo;

    {
        int check = pos;
        while (check < p->key_count && p->hashes[check] == hash) {
            if (data_record_key_matches(db, p->data_offsets[check], key, keylen)) {
                p->data_offsets[check] = data_off;
                return true;
            }
            check++;
        }
    }

    if (pos < p->key_count) {
        memmove(&p->hashes[pos + 1],       &p->hashes[pos],
                (size_t)(p->key_count - pos) * sizeof(uint64_t));
        memmove(&p->data_offsets[pos + 1],  &p->data_offsets[pos],
                (size_t)(p->key_count - pos) * sizeof(uint64_t));
    }

    p->hashes[pos]       = hash;
    p->data_offsets[pos] = data_off;
    p->key_count++;
    bloom_add(&p->bloom, hash);
    return true;
}

/*============================================================================
 * GET — acquires a reader slot for multi-process safety, then does the
 * standard skip-list descent + Bloom + SIMD scan + key verify.
 *============================================================================*/

SpikeDB_Status spike_db_get(SpikeDB* db, const char* key, size_t keylen,
                             char** val_out, size_t* vallen_out) {
    /* Acquire a reader slot so the writer won't reclaim pages we're reading */
    int rslot = db->exclusive ? -1 : reader_acquire(db);

    uint64_t hash = spikedb_hash(key, keylen);
    uint32_t pid = db->meta[db->active_meta]->root_page_id;

    if (pid == SPIKEDB_INVALID_PAGE) {
        reader_release(db, rslot);
        return SPIKEDB_NOT_FOUND;
    }

    SpikeDB_Page* p = page_ptr(db, pid);
    int level = (int)p->level - 1;
    if (level < 0) level = 0;

    while (level > 0) {
        uint32_t next = p->next_page_ids[level];
        if (next != SPIKEDB_INVALID_PAGE) {
            SpikeDB_Page* np = page_ptr(db, next);
            if (np->key_count > 0 && np->hashes[0] <= hash) {
                pid = next;
                p = np;
                continue;
            }
        }
        level--;
    }

    while (pid != SPIKEDB_INVALID_PAGE) {
        p = page_ptr(db, pid);
        if (p->key_count > 0 && p->hashes[0] > hash) {
            reader_release(db, rslot);
            return SPIKEDB_NOT_FOUND;
        }
        if (!db->ops.bloom_check(&p->bloom, hash)) {
            pid = p->next_page_ids[0];
            continue;
        }
        int idx = page_find_verified(db, p, hash, key, keylen);
        if (idx >= 0) {
            *val_out = data_record_read_value(db, p->data_offsets[idx], vallen_out);
            reader_release(db, rslot);
            if (!*val_out) return SPIKEDB_ERROR;
            return SPIKEDB_OK;
        }
        pid = p->next_page_ids[0];
    }

    reader_release(db, rslot);
    return SPIKEDB_NOT_FOUND;
}

/*============================================================================
 * Internal put/delete — called while holding the write mutex.
 * These modify the active meta page state in-place; the caller is responsible
 * for calling meta_commit() afterward.
 *============================================================================*/

static SpikeDB_Status spike_db_put_internal(SpikeDB* db, const char* key, size_t keylen,
                                             const char* val, size_t vallen) {
    if (keylen + vallen + 8 > SPIKEDB_DATA_PAGE_PAYLOAD)
        return SPIKEDB_ERROR;

    uint64_t hash = spikedb_hash(key, keylen);
    uint64_t data_off = alloc_data_record(db, key, keylen, val, vallen);
    if (data_off == UINT64_MAX) return SPIKEDB_FULL;

    SpikeDB_MetaPage* m = db->meta[db->active_meta];
    uint32_t root_pid = m->root_page_id;

    if (root_pid == SPIKEDB_INVALID_PAGE) {
        uint32_t new_pid = alloc_page(db);
        if (new_pid == SPIKEDB_INVALID_PAGE) return SPIKEDB_FULL;

        SpikeDB_Page* p = page_ptr(db, new_pid);
        page_init(p, SPIKEDB_MAX_LEVEL);
        p->hashes[0]       = hash;
        p->data_offsets[0]  = data_off;
        p->key_count        = 1;
        bloom_add(&p->bloom, hash);
        m->root_page_id = new_pid;
        return SPIKEDB_OK;
    }

    uint32_t update_pids[SPIKEDB_MAX_LEVEL];
    for (int i = 0; i < SPIKEDB_MAX_LEVEL; i++)
        update_pids[i] = SPIKEDB_INVALID_PAGE;

    SpikeDB_Page* root = page_ptr(db, root_pid);
    int max_lvl = (int)root->level - 1;
    if (max_lvl < 0) max_lvl = 0;

    uint32_t cur_pid = root_pid;
    SpikeDB_Page* cur = root;
    int level = max_lvl;

    while (level >= 0) {
        uint32_t next = cur->next_page_ids[level];
        if (next != SPIKEDB_INVALID_PAGE) {
            SpikeDB_Page* np = page_ptr(db, next);
            if (np->key_count > 0 && np->hashes[0] <= hash) {
                cur_pid = next;
                cur = np;
                continue;
            }
        }
        update_pids[level] = cur_pid;
        level--;
    }

    cur = page_ptr(db, cur_pid);

    int idx = page_find_verified(db, cur, hash, key, keylen);
    if (idx >= 0) {
        cur->data_offsets[idx] = data_off;
        return SPIKEDB_OK;
    }

    if (cur->key_count < SPIKEDB_KEYS_PER_PAGE) {
        page_insert_entry(db, cur, hash, data_off, key, keylen);
        return SPIKEDB_OK;
    }

    /* Page full -> split */
    {
        uint32_t new_pid = alloc_page(db);
        if (new_pid == SPIKEDB_INVALID_PAGE) return SPIKEDB_FULL;

        SpikeDB_Page* np = page_ptr(db, new_pid);
        uint16_t new_level = random_level();
        page_init(np, new_level);

        int split = cur->key_count / 2;
        int move_count = cur->key_count - split;
        memcpy(np->hashes,       &cur->hashes[split],       (size_t)move_count * sizeof(uint64_t));
        memcpy(np->data_offsets, &cur->data_offsets[split],  (size_t)move_count * sizeof(uint64_t));
        np->key_count = (uint16_t)move_count;

        for (int i = 0; i < np->key_count; i++)
            bloom_add(&np->bloom, np->hashes[i]);

        cur->key_count = (uint16_t)split;
        memset(&cur->bloom, 0, sizeof(SpikeDB_BloomBlock));
        for (int i = 0; i < cur->key_count; i++)
            bloom_add(&cur->bloom, cur->hashes[i]);

        np->next_page_ids[0] = cur->next_page_ids[0];
        cur->next_page_ids[0] = new_pid;

        for (int lv = 1; lv < (int)new_level && lv < SPIKEDB_MAX_LEVEL; lv++) {
            uint32_t upid = update_pids[lv];
            if (upid != SPIKEDB_INVALID_PAGE) {
                SpikeDB_Page* up = page_ptr(db, upid);
                np->next_page_ids[lv] = up->next_page_ids[lv];
                up->next_page_ids[lv] = new_pid;
            } else {
                SpikeDB_Page* rp = page_ptr(db, root_pid);
                if (lv < (int)rp->level) {
                    np->next_page_ids[lv] = rp->next_page_ids[lv];
                    rp->next_page_ids[lv] = new_pid;
                }
            }
        }

        if (np->key_count > 0 && hash >= np->hashes[0])
            page_insert_entry(db, np, hash, data_off, key, keylen);
        else
            page_insert_entry(db, cur, hash, data_off, key, keylen);

        return SPIKEDB_OK;
    }
}

static SpikeDB_Status spike_db_delete_internal(SpikeDB* db, const char* key, size_t keylen) {
    uint64_t hash = spikedb_hash(key, keylen);
    uint32_t root_pid = db->meta[db->active_meta]->root_page_id;
    if (root_pid == SPIKEDB_INVALID_PAGE) return SPIKEDB_NOT_FOUND;

    SpikeDB_Page* root = page_ptr(db, root_pid);
    int level = (int)root->level - 1;
    if (level < 0) level = 0;

    uint32_t cur_pid = root_pid;
    SpikeDB_Page* cur = root;

    while (level > 0) {
        uint32_t next = cur->next_page_ids[level];
        if (next != SPIKEDB_INVALID_PAGE) {
            SpikeDB_Page* np = page_ptr(db, next);
            if (np->key_count > 0 && np->hashes[0] <= hash) {
                cur_pid = next;
                cur = np;
                continue;
            }
        }
        level--;
    }

    while (cur_pid != SPIKEDB_INVALID_PAGE) {
        cur = page_ptr(db, cur_pid);
        if (cur->key_count > 0 && cur->hashes[0] > hash)
            return SPIKEDB_NOT_FOUND;

        int idx = page_find_verified(db, cur, hash, key, keylen);
        if (idx >= 0) {
            int remaining = cur->key_count - idx - 1;
            if (remaining > 0) {
                memmove(&cur->hashes[idx],       &cur->hashes[idx + 1],
                        (size_t)remaining * sizeof(uint64_t));
                memmove(&cur->data_offsets[idx],  &cur->data_offsets[idx + 1],
                        (size_t)remaining * sizeof(uint64_t));
            }
            cur->key_count--;
            memset(&cur->bloom, 0, sizeof(SpikeDB_BloomBlock));
            for (int i = 0; i < cur->key_count; i++)
                bloom_add(&cur->bloom, cur->hashes[i]);
            return SPIKEDB_OK;
        }
        cur_pid = cur->next_page_ids[0];
    }
    return SPIKEDB_NOT_FOUND;
}

/*============================================================================
 * Public PUT / DELETE — acquire write mutex, perform operation, commit meta
 *============================================================================*/

SpikeDB_Status spike_db_put(SpikeDB* db, const char* key, size_t keylen,
                             const char* val, size_t vallen) {
    if (!db->exclusive) write_lock(db);
    SpikeDB_Status rc = spike_db_put_internal(db, key, keylen, val, vallen);
    if (rc == SPIKEDB_OK)
        meta_commit(db);
    if (!db->exclusive) write_unlock(db);
    return rc;
}

SpikeDB_Status spike_db_delete(SpikeDB* db, const char* key, size_t keylen) {
    if (!db->exclusive) write_lock(db);
    SpikeDB_Status rc = spike_db_delete_internal(db, key, keylen);
    if (rc == SPIKEDB_OK)
        meta_commit(db);
    if (!db->exclusive) write_unlock(db);
    return rc;
}

/*============================================================================
 * spike_db_free
 *============================================================================*/

void spike_db_free(void* ptr) {
    free(ptr);
}

/*============================================================================
 * WriteBatch — holds write mutex for the entire batch, commits once at end
 *============================================================================*/

SpikeDB_WriteBatch* spike_db_writebatch_create(void) {
    SpikeDB_WriteBatch* b = (SpikeDB_WriteBatch*)calloc(1, sizeof(SpikeDB_WriteBatch));
    if (!b) return NULL;
    b->capacity = 4096;
    b->data = (uint8_t*)malloc(b->capacity);
    if (!b->data) { free(b); return NULL; }
    return b;
}

void spike_db_writebatch_destroy(SpikeDB_WriteBatch* batch) {
    if (!batch) return;
    free(batch->data);
    free(batch);
}

static void writebatch_ensure(SpikeDB_WriteBatch* b, size_t needed) {
    while (b->size + needed > b->capacity) {
        b->capacity *= 2;
        b->data = (uint8_t*)realloc(b->data, b->capacity);
    }
}

void spike_db_writebatch_put(SpikeDB_WriteBatch* batch,
                              const char* key, size_t keylen,
                              const char* val, size_t vallen) {
    size_t needed = 1 + 4 + 4 + keylen + vallen;
    writebatch_ensure(batch, needed);
    uint8_t* p = batch->data + batch->size;
    *p++ = SPIKEDB_BATCH_OP_PUT;
    { uint32_t kl = (uint32_t)keylen, vl = (uint32_t)vallen;
      memcpy(p, &kl, 4); p += 4; memcpy(p, &vl, 4); p += 4; }
    if (keylen > 0) { memcpy(p, key, keylen); p += keylen; }
    if (vallen > 0) { memcpy(p, val, vallen); p += vallen; }
    batch->size += needed;
    batch->count++;
}

void spike_db_writebatch_delete(SpikeDB_WriteBatch* batch,
                                 const char* key, size_t keylen) {
    size_t needed = 1 + 4 + 4 + keylen;
    writebatch_ensure(batch, needed);
    uint8_t* p = batch->data + batch->size;
    *p++ = SPIKEDB_BATCH_OP_DELETE;
    { uint32_t kl = (uint32_t)keylen, vl = 0;
      memcpy(p, &kl, 4); p += 4; memcpy(p, &vl, 4); p += 4; }
    if (keylen > 0) { memcpy(p, key, keylen); p += keylen; }
    batch->size += needed;
    batch->count++;
}

void spike_db_writebatch_clear(SpikeDB_WriteBatch* batch) {
    batch->size  = 0;
    batch->count = 0;
}

int spike_db_writebatch_count(const SpikeDB_WriteBatch* batch) {
    return batch->count;
}

/* Apply all operations in the batch under a single write mutex + single commit */
SpikeDB_Status spike_db_write(SpikeDB* db, SpikeDB_WriteBatch* batch) {
    if (!db->exclusive) write_lock(db);

    const uint8_t* p   = batch->data;
    const uint8_t* end = batch->data + batch->size;
    bool any_ok = false;

    while (p < end) {
        uint8_t op = *p++;
        uint32_t kl, vl;
        memcpy(&kl, p, 4); p += 4;
        memcpy(&vl, p, 4); p += 4;
        const char* key = (const char*)p; p += kl;
        const char* val = (const char*)p; p += vl;

        SpikeDB_Status rc;
        if (op == SPIKEDB_BATCH_OP_PUT)
            rc = spike_db_put_internal(db, key, (size_t)kl, val, (size_t)vl);
        else
            rc = spike_db_delete_internal(db, key, (size_t)kl);

        if (rc == SPIKEDB_OK)
            any_ok = true;
        else if (rc != SPIKEDB_NOT_FOUND) {
            if (!db->exclusive) write_unlock(db);
            return rc;
        }
    }

    /* Single meta commit for the entire batch */
    if (any_ok)
        meta_commit(db);

    if (!db->exclusive) write_unlock(db);
    return SPIKEDB_OK;
}
