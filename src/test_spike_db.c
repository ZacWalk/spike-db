/*============================================================================
 * test_spike_db.c  –  Comprehensive test harness for SpikeDB
 *
 * Tests variable-length key/value API, WriteBatch, and SIMD-accelerated
 * skip-list operations.
 *============================================================================*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spike_db.h"

#define TEST_DB_PATH "test_spike_db.dat"

/* ---- Minimal test framework ---- */
static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define CHECK(cond, fmt, ...) do {                                          \
    if (!(cond)) {                                                          \
        printf("    FAIL: " fmt "\n", ##__VA_ARGS__);                       \
        g_tests_failed++; g_tests_run++; return;                            \
    }                                                                       \
} while(0)

#define CHECK_CONTINUE(cond, counter, fmt, ...) do {                        \
    if (!(cond)) { counter++; if (counter <= 5) printf("    FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

#define PASS() do { g_tests_passed++; g_tests_run++; printf("    PASS\n"); } while(0)

static void cleanup(void) {
    remove(TEST_DB_PATH);
    remove(TEST_DB_PATH ".lock");
}

static const char* simd_name(SpikeDB_SimdLevel lvl) {
    switch (lvl) {
    case SPIKEDB_SIMD_AVX512: return "AVX-512";
    case SPIKEDB_SIMD_AVX2:   return "AVX2";
    default:                   return "Scalar";
    }
}

/* ---- Timer helper ---- */
#ifdef _WIN32
static LARGE_INTEGER g_freq;
static void timer_init(void) { QueryPerformanceFrequency(&g_freq); }
typedef LARGE_INTEGER SpikeDB_Timer;
static void timer_now(SpikeDB_Timer* t) { QueryPerformanceCounter(t); }
static double timer_ms(SpikeDB_Timer t0, SpikeDB_Timer t1) {
    return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / g_freq.QuadPart;
}
#else
static void timer_init(void) { }
typedef struct timespec SpikeDB_Timer;
static void timer_now(SpikeDB_Timer* t) { clock_gettime(CLOCK_MONOTONIC, t); }
static double timer_ms(SpikeDB_Timer t0, SpikeDB_Timer t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}
#endif

/* ---- PRNG ---- */
static uint64_t xorshift64(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *state = x;
    return x;
}

/*============================================================================
 * uint64 convenience wrappers — existing tests use integer keys/values.
 * These convert to/from string representations under the new byte-key API.
 *============================================================================*/

static SpikeDB_Status put_u64(SpikeDB* db, uint64_t key, uint64_t value) {
    char k[32], v[32];
    int kl = sprintf(k, "%llu", (unsigned long long)key);
    int vl = sprintf(v, "%llu", (unsigned long long)value);
    return spike_db_put(db, k, (size_t)kl, v, (size_t)vl);
}

static SpikeDB_Status get_u64(SpikeDB* db, uint64_t key, uint64_t* value_out) {
    char k[32];
    int kl = sprintf(k, "%llu", (unsigned long long)key);
    char* val = NULL;
    size_t vlen = 0;
    SpikeDB_Status rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
    if (rc == SPIKEDB_OK && val) {
        /* Null-terminate for strtoull */
        char tmp[32] = {0};
        size_t cplen = vlen < 31 ? vlen : 31;
        memcpy(tmp, val, cplen);
        *value_out = strtoull(tmp, NULL, 10);
        spike_db_free(val);
    }
    return rc;
}

static SpikeDB_Status del_u64(SpikeDB* db, uint64_t key) {
    char k[32];
    int kl = sprintf(k, "%llu", (unsigned long long)key);
    return spike_db_delete(db, k, (size_t)kl);
}

/*============================================================================
 * TEST 1: Basic CRUD (put, get, update, delete, miss)
 *============================================================================*/
static void test_basic_crud(void) {
    printf("  [1] Basic CRUD\n");
    cleanup();
    SpikeDB* db = NULL;
    SpikeDB_Status rc = spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(rc == SPIKEDB_OK && db, "open failed rc=%d", rc);
    printf("    SIMD level: %s\n", simd_name(spike_db_simd_level(db)));

    for (uint64_t i = 1; i <= 100; i++) {
        rc = put_u64(db, i, i * 1000);
        CHECK(rc == SPIKEDB_OK, "put %llu failed", (unsigned long long)i);
    }

    uint64_t val;
    int fails = 0;
    for (uint64_t i = 1; i <= 100; i++) {
        rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i * 1000, fails,
                        "get key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    CHECK(fails == 0, "%d / 100 gets failed", fails);

    /* Update */
    rc = put_u64(db, 42, 99999);
    CHECK(rc == SPIKEDB_OK, "update put failed");
    rc = get_u64(db, 42, &val);
    CHECK(rc == SPIKEDB_OK && val == 99999, "update get failed val=%llu", (unsigned long long)val);

    /* Delete */
    rc = del_u64(db, 42);
    CHECK(rc == SPIKEDB_OK, "delete failed");
    rc = get_u64(db, 42, &val);
    CHECK(rc == SPIKEDB_NOT_FOUND, "deleted key still found");

    /* Miss */
    rc = get_u64(db, 9999, &val);
    CHECK(rc == SPIKEDB_NOT_FOUND, "miss key found");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 2: Page-boundary crossing (>64 keys forces multi-page)
 *============================================================================*/
static void test_page_boundary(void) {
    printf("  [2] Page boundary (200 keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 200; i++)
        put_u64(db, i, i + 0xAAAA);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 200; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i + 0xAAAA, fails,
                        "key=%llu rc=%d", (unsigned long long)i, rc);
    }
    CHECK(fails == 0, "%d / 200 gets failed", fails);
    printf("    200 keys across pages: OK\n");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 3: Large sequential insert + verify (10k)
 *============================================================================*/
static void test_large_sequential(void) {
    printf("  [3] Large sequential (10,000 keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const uint64_t N = 10000;
    int put_ok = 0;
    for (uint64_t i = 0; i < N; i++)
        if (put_u64(db, i, i * 7 + 3) == SPIKEDB_OK) put_ok++;

    printf("    put: %d/%llu\n", put_ok, (unsigned long long)N);
    CHECK(put_ok == (int)N, "only %d puts succeeded", put_ok);

    int get_ok = 0;
    uint64_t val;
    for (uint64_t i = 0; i < N; i++)
        if (get_u64(db, i, &val) == SPIKEDB_OK && val == i * 7 + 3) get_ok++;

    printf("    get: %d/%llu\n", get_ok, (unsigned long long)N);
    CHECK(get_ok == (int)N, "only %d gets succeeded", get_ok);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 4: Reverse-order insertion (stress page splitting)
 *============================================================================*/
static void test_reverse_insert(void) {
    printf("  [4] Reverse-order insert (5,000 keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const uint64_t N = 5000;
    for (uint64_t i = N; i >= 1; i--)
        put_u64(db, i, i * 11);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 1; i <= N; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i * 11, fails,
                        "key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    printf("    verified: %llu/%llu\n", (unsigned long long)(N - fails), (unsigned long long)N);
    CHECK(fails == 0, "%d gets failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 5: Random-order insertion
 *============================================================================*/
static void test_random_insert(void) {
    printf("  [5] Random-order insert (10,000 keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const int N = 10000;
    uint64_t* keys = (uint64_t*)malloc(N * sizeof(uint64_t));
    CHECK(keys, "malloc failed");
    for (int i = 0; i < N; i++) keys[i] = (uint64_t)(i + 1);
    uint64_t rng = 0xDEADBEEFCAFEULL;
    for (int i = N - 1; i > 0; i--) {
        int j = (int)(xorshift64(&rng) % (uint64_t)(i + 1));
        uint64_t tmp = keys[i]; keys[i] = keys[j]; keys[j] = tmp;
    }

    for (int i = 0; i < N; i++)
        put_u64(db, keys[i], keys[i] + 0xF00D);

    int fails = 0;
    uint64_t val;
    for (int i = 0; i < N; i++) {
        SpikeDB_Status rc = get_u64(db, keys[i], &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == keys[i] + 0xF00D, fails,
                        "key=%llu rc=%d", (unsigned long long)keys[i], rc);
    }
    printf("    verified: %d/%d\n", N - fails, N);
    free(keys);
    CHECK(fails == 0, "%d random gets failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 6: Overwrite / update stress
 *============================================================================*/
static void test_overwrite_stress(void) {
    printf("  [6] Overwrite stress (1,000 keys x 10 updates)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const uint64_t N = 1000;
    const int ROUNDS = 10;

    for (int r = 0; r < ROUNDS; r++)
        for (uint64_t i = 0; i < N; i++)
            put_u64(db, i, i + (uint64_t)r * 1000);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < N; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        uint64_t expected = i + (uint64_t)(ROUNDS - 1) * 1000;
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == expected, fails,
                        "key=%llu expected=%llu got=%llu", (unsigned long long)i,
                        (unsigned long long)expected, (unsigned long long)val);
    }
    printf("    verified final values: %llu/%llu\n", (unsigned long long)(N - fails), (unsigned long long)N);
    CHECK(fails == 0, "%d overwrite checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 7: Delete-heavy workload
 *============================================================================*/
static void test_delete_heavy(void) {
    printf("  [7] Delete-heavy (insert 1000, delete odd, verify)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 1; i <= 1000; i++)
        put_u64(db, i, i * 3);

    int del_ok = 0;
    for (uint64_t i = 1; i <= 1000; i += 2)
        if (del_u64(db, i) == SPIKEDB_OK) del_ok++;
    printf("    deleted %d odd keys\n", del_ok);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 1; i <= 1000; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        if (i % 2 == 1) {
            CHECK_CONTINUE(rc == SPIKEDB_NOT_FOUND, fails,
                            "odd key=%llu still found", (unsigned long long)i);
        } else {
            CHECK_CONTINUE(rc == SPIKEDB_OK && val == i * 3, fails,
                            "even key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
        }
    }
    printf("    verification errors: %d\n", fails);
    CHECK(fails == 0, "%d delete checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 8: Delete non-existent key
 *============================================================================*/
static void test_delete_missing(void) {
    printf("  [8] Delete non-existent key\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    put_u64(db, 1, 100);
    SpikeDB_Status rc = del_u64(db, 999);
    CHECK(rc == SPIKEDB_NOT_FOUND, "expected NOT_FOUND, got %d", rc);

    uint64_t val;
    rc = get_u64(db, 1, &val);
    CHECK(rc == SPIKEDB_OK && val == 100, "original key damaged");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 9: Empty database operations
 *============================================================================*/
static void test_empty_db(void) {
    printf("  [9] Empty database ops\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    uint64_t val;
    SpikeDB_Status rc = get_u64(db, 1, &val);
    CHECK(rc == SPIKEDB_NOT_FOUND, "get on empty db didn't return NOT_FOUND");

    rc = del_u64(db, 1);
    CHECK(rc == SPIKEDB_NOT_FOUND, "delete on empty db didn't return NOT_FOUND");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 10: Re-open persistence
 *============================================================================*/
static void test_reopen(void) {
    printf("  [10] Close and re-open persistence\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 500; i++)
        put_u64(db, i, i + 0xCAFE);

    spike_db_close(db);
    db = NULL;

    SpikeDB_Status rc = spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(rc == SPIKEDB_OK && db, "re-open failed rc=%d", rc);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 500; i++) {
        rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i + 0xCAFE, fails,
                        "key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    printf("    after reopen: %d/500\n", 500 - fails);
    CHECK(fails == 0, "%d keys lost after reopen", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 11: Variable-length string keys and values
 *============================================================================*/
static void test_string_keys(void) {
    printf("  [11] Variable-length string keys\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const char* keys[] = { "a", "hello", "this is a longer key",
                            "key with special chars: !@#$%^&*()", "" };
    const char* vals[] = { "1", "world", "this is a longer value",
                           "value!!", "empty key value" };
    int n = sizeof(keys) / sizeof(keys[0]);

    for (int i = 0; i < n; i++) {
        SpikeDB_Status rc = spike_db_put(db, keys[i], strlen(keys[i]),
                                          vals[i], strlen(vals[i]));
        CHECK(rc == SPIKEDB_OK, "put failed for key '%s'", keys[i]);
    }

    int fails = 0;
    for (int i = 0; i < n; i++) {
        char* val = NULL;
        size_t vlen = 0;
        SpikeDB_Status rc = spike_db_get(db, keys[i], strlen(keys[i]), &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != strlen(vals[i]) ||
            memcmp(val, vals[i], vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d string key checks failed", fails);

    /* Update a string key */
    {
        SpikeDB_Status rc = spike_db_put(db, "hello", 5, "updated", 7);
        CHECK(rc == SPIKEDB_OK, "string update failed");
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, "hello", 5, &val, &vlen);
        CHECK(rc == SPIKEDB_OK && vlen == 7 && memcmp(val, "updated", 7) == 0,
              "string update verify failed");
        spike_db_free(val);
    }

    /* Delete a string key */
    {
        SpikeDB_Status rc = spike_db_delete(db, "a", 1);
        CHECK(rc == SPIKEDB_OK, "string delete failed");
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, "a", 1, &val, &vlen);
        CHECK(rc == SPIKEDB_NOT_FOUND, "deleted string key still found");
        spike_db_free(val);
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 12: Interleaved insert-delete
 *============================================================================*/
static void test_interleaved_insert_delete(void) {
    printf("  [12] Interleaved insert-delete (2,000 ops)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 1000; i++)
        put_u64(db, i, i);

    for (uint64_t i = 0; i < 1000; i += 2) {
        del_u64(db, i);
        put_u64(db, i + 1000, (i + 1000) * 5);
    }

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 1000; i += 2) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_NOT_FOUND, fails,
                        "deleted even key=%llu still found", (unsigned long long)i);
    }
    for (uint64_t i = 1; i < 1000; i += 2) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i, fails,
                        "odd key=%llu missing", (unsigned long long)i);
    }
    for (uint64_t i = 0; i < 1000; i += 2) {
        SpikeDB_Status rc = get_u64(db, i + 1000, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == (i + 1000) * 5, fails,
                        "new key=%llu missing", (unsigned long long)(i + 1000));
    }
    printf("    interleave errors: %d\n", fails);
    CHECK(fails == 0, "%d interleave checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 13: Sparse key space (large gaps)
 *============================================================================*/
static void test_sparse_keys(void) {
    printf("  [13] Sparse key space (keys with large gaps)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const int N = 100;
    for (int i = 0; i < N; i++)
        put_u64(db, (uint64_t)i * 1000, (uint64_t)i * 1000 + 7);

    int fails = 0;
    uint64_t val;
    for (int i = 0; i < N; i++) {
        SpikeDB_Status rc = get_u64(db, (uint64_t)i * 1000, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == (uint64_t)i * 1000 + 7, fails,
                        "sparse key=%llu rc=%d", (unsigned long long)((uint64_t)i * 1000), rc);
    }
    for (int i = 0; i < N; i++) {
        SpikeDB_Status rc = get_u64(db, (uint64_t)i * 1000 + 500, &val);
        CHECK_CONTINUE(rc == SPIKEDB_NOT_FOUND, fails,
                        "gap key=%llu found", (unsigned long long)((uint64_t)i * 1000 + 500));
    }
    printf("    sparse errors: %d\n", fails);
    CHECK(fails == 0, "%d sparse checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 14: Same key repeated insert (idempotency)
 *============================================================================*/
static void test_same_key_repeat(void) {
    printf("  [14] Same key repeated insert (idempotency)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (int i = 0; i < 100; i++)
        put_u64(db, 42, (uint64_t)(i + 1));

    uint64_t val;
    SpikeDB_Status rc = get_u64(db, 42, &val);
    CHECK(rc == SPIKEDB_OK && val == 100, "expected 100, got %llu", (unsigned long long)val);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 15: Delete all keys -> empty database
 *============================================================================*/
static void test_delete_all(void) {
    printf("  [15] Delete all keys\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 200; i++)
        put_u64(db, i, i);

    for (uint64_t i = 0; i < 200; i++)
        del_u64(db, i);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 200; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_NOT_FOUND, fails,
                        "key=%llu still found after delete-all", (unsigned long long)i);
    }
    CHECK(fails == 0, "%d keys survived delete-all", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 16: Insert after delete-all (freelist reuse)
 *============================================================================*/
static void test_reinsert_after_delete(void) {
    printf("  [16] Re-insert after delete-all\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 100; i++)
        put_u64(db, i, i);
    for (uint64_t i = 0; i < 100; i++)
        del_u64(db, i);

    for (uint64_t i = 0; i < 100; i++)
        put_u64(db, i, i + 9999);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 100; i++) {
        SpikeDB_Status rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i + 9999, fails,
                        "key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    CHECK(fails == 0, "%d re-insert checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 17: Performance - sequential 100k
 *============================================================================*/
static void test_perf_sequential(void) {
    printf("  [17] Perf: sequential 100k\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
    CHECK(db, "open failed");

    const uint64_t N = 100000;
    SpikeDB_Timer t0, t1;

    timer_now(&t0);
    for (uint64_t i = 0; i < N; i++)
        put_u64(db, i, i);
    timer_now(&t1);
    double put_ms = timer_ms(t0, t1);

    uint64_t v;
    int hits = 0;
    timer_now(&t0);
    for (uint64_t i = 0; i < N; i++)
        if (get_u64(db, i, &v) == SPIKEDB_OK) hits++;
    timer_now(&t1);
    double get_ms = timer_ms(t0, t1);

    printf("    put: %.1f ms  (%.0f ops/s)\n", put_ms, N / (put_ms / 1000.0));
    printf("    get: %.1f ms  (%.0f ops/s)  hits=%d/%llu\n", get_ms, N / (get_ms / 1000.0), hits, (unsigned long long)N);
    CHECK(hits == (int)N, "only %d/%llu hits", hits, (unsigned long long)N);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 18: Performance - random 100k
 *============================================================================*/
static void test_perf_random(void) {
    printf("  [18] Perf: random-order 100k\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
    CHECK(db, "open failed");

    const int N = 100000;
    uint64_t* keys = (uint64_t*)malloc(N * sizeof(uint64_t));
    CHECK(keys, "malloc failed");
    for (int i = 0; i < N; i++) keys[i] = (uint64_t)i;
    uint64_t rng = 0x12345678ABCDULL;
    for (int i = N - 1; i > 0; i--) {
        int j = (int)(xorshift64(&rng) % (uint64_t)(i + 1));
        uint64_t tmp = keys[i]; keys[i] = keys[j]; keys[j] = tmp;
    }

    SpikeDB_Timer t0, t1;

    timer_now(&t0);
    for (int i = 0; i < N; i++)
        put_u64(db, keys[i], keys[i]);
    timer_now(&t1);
    double put_ms = timer_ms(t0, t1);

    for (int i = N - 1; i > 0; i--) {
        int j = (int)(xorshift64(&rng) % (uint64_t)(i + 1));
        uint64_t tmp = keys[i]; keys[i] = keys[j]; keys[j] = tmp;
    }

    uint64_t v;
    int hits = 0;
    timer_now(&t0);
    for (int i = 0; i < N; i++)
        if (get_u64(db, keys[i], &v) == SPIKEDB_OK) hits++;
    timer_now(&t1);
    double get_ms = timer_ms(t0, t1);

    printf("    put: %.1f ms  (%.0f ops/s)\n", put_ms, N / (put_ms / 1000.0));
    printf("    get: %.1f ms  (%.0f ops/s)  hits=%d/%d\n", get_ms, N / (get_ms / 1000.0), hits, N);
    CHECK(hits == N, "only %d/%d hits", hits, N);

    free(keys);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 19: Performance - point delete throughput
 *============================================================================*/
static void test_perf_delete(void) {
    printf("  [19] Perf: delete 50k after insert 50k\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
    CHECK(db, "open failed");

    const uint64_t N = 50000;
    for (uint64_t i = 0; i < N; i++)
        put_u64(db, i, i);

    SpikeDB_Timer t0, t1;
    int del_ok = 0;
    timer_now(&t0);
    for (uint64_t i = 0; i < N; i++)
        if (del_u64(db, i) == SPIKEDB_OK) del_ok++;
    timer_now(&t1);
    double del_ms = timer_ms(t0, t1);

    printf("    delete: %.1f ms  (%.0f ops/s)  deleted=%d/%llu\n",
           del_ms, N / (del_ms / 1000.0), del_ok, (unsigned long long)N);
    CHECK(del_ok == (int)N, "only %d/%llu deletes", del_ok, (unsigned long long)N);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 20: Scale test - 500k keys
 *============================================================================*/
static void test_scale_500k(void) {
    printf("  [20] Scale: 500k sequential keys\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
    CHECK(db, "open failed");

    const uint64_t N = 500000;
    SpikeDB_Timer t0, t1;

    timer_now(&t0);
    for (uint64_t i = 0; i < N; i++)
        put_u64(db, i, i);
    timer_now(&t1);
    double put_ms = timer_ms(t0, t1);
    printf("    put: %.1f ms  (%.0f ops/s)\n", put_ms, N / (put_ms / 1000.0));

    uint64_t rng = 0xABCDABCDABCDULL;
    int hits = 0;
    const int SAMPLE = 10000;
    timer_now(&t0);
    for (int i = 0; i < SAMPLE; i++) {
        uint64_t k = xorshift64(&rng) % N;
        uint64_t v;
        if (get_u64(db, k, &v) == SPIKEDB_OK) hits++;
    }
    timer_now(&t1);
    double get_ms = timer_ms(t0, t1);
    printf("    random sample get (%d): %.1f ms  (%.0f ops/s)  hits=%d\n",
           SAMPLE, get_ms, SAMPLE / (get_ms / 1000.0), hits);
    CHECK(hits == SAMPLE, "only %d/%d sample hits", hits, SAMPLE);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 21: WriteBatch — batched puts and deletes
 *============================================================================*/
static void test_writebatch(void) {
    printf("  [21] WriteBatch\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    SpikeDB_WriteBatch* batch = spike_db_writebatch_create();
    CHECK(batch, "writebatch create failed");

    /* Batch 100 puts */
    for (int i = 0; i < 100; i++) {
        char k[32], v[32];
        int kl = sprintf(k, "batch_key_%d", i);
        int vl = sprintf(v, "batch_val_%d", i);
        spike_db_writebatch_put(batch, k, (size_t)kl, v, (size_t)vl);
    }
    CHECK(spike_db_writebatch_count(batch) == 100, "batch count wrong");

    SpikeDB_Status rc = spike_db_write(db, batch);
    CHECK(rc == SPIKEDB_OK, "write batch failed rc=%d", rc);

    /* Verify all 100 keys exist */
    int fails = 0;
    for (int i = 0; i < 100; i++) {
        char k[32], expected[32];
        int kl = sprintf(k, "batch_key_%d", i);
        int el = sprintf(expected, "batch_val_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d batch get failures", fails);

    /* Mixed batch: delete first 50, insert 50 new */
    spike_db_writebatch_clear(batch);
    CHECK(spike_db_writebatch_count(batch) == 0, "clear didn't reset count");

    for (int i = 0; i < 50; i++) {
        char k[32];
        int kl = sprintf(k, "batch_key_%d", i);
        spike_db_writebatch_delete(batch, k, (size_t)kl);
    }
    for (int i = 100; i < 150; i++) {
        char k[32], v[32];
        int kl = sprintf(k, "batch_key_%d", i);
        int vl = sprintf(v, "batch_val_%d", i);
        spike_db_writebatch_put(batch, k, (size_t)kl, v, (size_t)vl);
    }

    rc = spike_db_write(db, batch);
    CHECK(rc == SPIKEDB_OK, "mixed batch failed rc=%d", rc);

    /* Verify deletes */
    fails = 0;
    for (int i = 0; i < 50; i++) {
        char k[32];
        int kl = sprintf(k, "batch_key_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_NOT_FOUND) fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d should-be-deleted keys found", fails);

    /* Verify surviving + new keys */
    fails = 0;
    for (int i = 50; i < 150; i++) {
        char k[32], expected[32];
        int kl = sprintf(k, "batch_key_%d", i);
        int el = sprintf(expected, "batch_val_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d post-batch checks failed", fails);

    spike_db_writebatch_destroy(batch);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 22: Large binary key/value
 *============================================================================*/
static void test_binary_data(void) {
    printf("  [22] Binary keys and values\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    /* Key and value with embedded null bytes */
    const char bin_key[] = "key\x00with\x00nulls";
    size_t bin_keylen = sizeof(bin_key) - 1;  /* 14 bytes including nulls */
    const char bin_val[] = "val\x00\x01\x02\xFF\xFE";
    size_t bin_vallen = sizeof(bin_val) - 1;

    SpikeDB_Status rc = spike_db_put(db, bin_key, bin_keylen, bin_val, bin_vallen);
    CHECK(rc == SPIKEDB_OK, "binary put failed");

    char* val = NULL;
    size_t vlen = 0;
    rc = spike_db_get(db, bin_key, bin_keylen, &val, &vlen);
    CHECK(rc == SPIKEDB_OK, "binary get failed");
    CHECK(vlen == bin_vallen && memcmp(val, bin_val, vlen) == 0, "binary value mismatch");
    spike_db_free(val);

    /* Large value (2KB) */
    {
        char big_val[2048];
        memset(big_val, 0xAB, sizeof(big_val));
        rc = spike_db_put(db, "bigkey", 6, big_val, sizeof(big_val));
        CHECK(rc == SPIKEDB_OK, "large value put failed");

        val = NULL;
        rc = spike_db_get(db, "bigkey", 6, &val, &vlen);
        CHECK(rc == SPIKEDB_OK && vlen == sizeof(big_val), "large value get failed");
        int ok = 1;
        for (size_t i = 0; i < vlen; i++)
            if ((unsigned char)val[i] != 0xAB) { ok = 0; break; }
        CHECK(ok, "large value data corruption");
        spike_db_free(val);
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 23: Empty key and empty value
 *============================================================================*/
static void test_empty_key_value(void) {
    printf("  [23] Empty key and empty value\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    /* Empty value */
    SpikeDB_Status rc = spike_db_put(db, "novalue", 7, "", 0);
    CHECK(rc == SPIKEDB_OK, "empty value put failed");
    {
        char* val = NULL;
        size_t vlen = 99;
        rc = spike_db_get(db, "novalue", 7, &val, &vlen);
        CHECK(rc == SPIKEDB_OK && vlen == 0, "empty value get failed vlen=%zu", vlen);
        spike_db_free(val);
    }

    /* Empty key */
    rc = spike_db_put(db, "", 0, "has_value", 9);
    CHECK(rc == SPIKEDB_OK, "empty key put failed");
    {
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, "", 0, &val, &vlen);
        CHECK(rc == SPIKEDB_OK && vlen == 9 && memcmp(val, "has_value", 9) == 0,
              "empty key get failed");
        spike_db_free(val);
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 24: Meta page integrity (CRC32 validation on reopen)
 *============================================================================*/
static void test_meta_integrity(void) {
    printf("  [24] Meta page integrity (CRC32 + double-buffer)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    /* Write some data so txn_id advances */
    for (uint64_t i = 0; i < 50; i++)
        put_u64(db, i, i * 7);

    spike_db_close(db);
    db = NULL;

    /* Re-open — both meta pages should be valid, higher txn_id wins */
    SpikeDB_Status rc = spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(rc == SPIKEDB_OK && db, "reopen after writes failed rc=%d", rc);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 50; i++) {
        rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i * 7, fails,
                        "key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    CHECK(fails == 0, "%d meta-integrity checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 25: Simulated torn write recovery
 *
 * Corrupt one meta page's checksum, then re-open.  The database should
 * recover from the other (still-valid) meta page.
 *============================================================================*/
static void test_torn_write_recovery(void) {
    printf("  [25] Torn-write recovery (corrupt one meta page)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (uint64_t i = 0; i < 100; i++)
        put_u64(db, i, i + 0xBEEF);

    spike_db_close(db);
    db = NULL;

    /* Corrupt meta page 0 by flipping some bytes in its checksum area.
     * We do this by raw file I/O, not through the mmap. */
    {
#ifdef _WIN32
        HANDLE hFile = CreateFileA(TEST_DB_PATH, GENERIC_READ | GENERIC_WRITE,
                                    0, NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(hFile != INVALID_HANDLE_VALUE, "corrupt: open failed");

        /* Read page 0, trash the checksum, write back */
        uint8_t page[4096];
        DWORD read_bytes = 0;
        ReadFile(hFile, page, 4096, &read_bytes, NULL);
        CHECK(read_bytes == 4096, "corrupt: read failed");

        /* Flip bytes at offset 36 (checksum field) */
        page[36] ^= 0xFF;
        page[37] ^= 0xFF;

        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(hFile, page, 4096, &written, NULL);
        CloseHandle(hFile);
#else
        int fd = open(TEST_DB_PATH, O_RDWR);
        CHECK(fd >= 0, "corrupt: open failed");

        uint8_t page[4096];
        ssize_t rb = read(fd, page, 4096);
        CHECK(rb == 4096, "corrupt: read failed");

        page[36] ^= 0xFF;
        page[37] ^= 0xFF;

        lseek(fd, 0, SEEK_SET);
        ssize_t wb = write(fd, page, 4096);
        (void)wb;
        close(fd);
#endif
    }

    /* Re-open — should recover from meta page 1 */
    SpikeDB_Status rc = spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(rc == SPIKEDB_OK && db, "recovery open failed rc=%d", rc);

    int fails = 0;
    uint64_t val;
    for (uint64_t i = 0; i < 100; i++) {
        rc = get_u64(db, i, &val);
        CHECK_CONTINUE(rc == SPIKEDB_OK && val == i + 0xBEEF, fails,
                        "key=%llu rc=%d val=%llu", (unsigned long long)i, rc, (unsigned long long)val);
    }
    printf("    recovered %d/100 keys from surviving meta page\n", 100 - fails);
    CHECK(fails == 0, "%d recovery checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 26: WriteBatch atomicity (single commit for batch)
 *============================================================================*/
static void test_writebatch_single_commit(void) {
    printf("  [26] WriteBatch single commit\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    /* Put 200 keys in a batch — should result in exactly 1 meta commit */
    SpikeDB_WriteBatch* batch = spike_db_writebatch_create();
    CHECK(batch, "batch create failed");

    for (int i = 0; i < 200; i++) {
        char k[32], v[32];
        int kl = sprintf(k, "batch2_%d", i);
        int vl = sprintf(v, "val2_%d", i);
        spike_db_writebatch_put(batch, k, (size_t)kl, v, (size_t)vl);
    }

    SpikeDB_Status rc = spike_db_write(db, batch);
    CHECK(rc == SPIKEDB_OK, "batch write failed rc=%d", rc);
    spike_db_writebatch_destroy(batch);

    spike_db_close(db);
    db = NULL;

    /* Reopen and verify */
    rc = spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(rc == SPIKEDB_OK && db, "reopen failed");

    int fails = 0;
    for (int i = 0; i < 200; i++) {
        char k[32], expected[32];
        int kl = sprintf(k, "batch2_%d", i);
        int el = sprintf(expected, "val2_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d batch single-commit checks failed", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 27: Exactly 64 keys per page (page-full boundary)
 *============================================================================*/
static void test_exact_page_fill(void) {
    printf("  [27] Exact page fill (64 keys = 1 full page)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    for (int i = 0; i < SPIKEDB_KEYS_PER_PAGE; i++) {
        char k[32], v[32];
        int kl = sprintf(k, "fill_%d", i);
        int vl = sprintf(v, "val_%d", i);
        SpikeDB_Status rc = spike_db_put(db, k, (size_t)kl, v, (size_t)vl);
        CHECK(rc == SPIKEDB_OK, "put %d failed", i);
    }

    int fails = 0;
    for (int i = 0; i < SPIKEDB_KEYS_PER_PAGE; i++) {
        char k[32], expected[32];
        int kl = sprintf(k, "fill_%d", i);
        int el = sprintf(expected, "val_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        SpikeDB_Status rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d / %d page-fill checks failed", fails, SPIKEDB_KEYS_PER_PAGE);

    /* Insert one more to trigger page split */
    {
        SpikeDB_Status rc = spike_db_put(db, "split_trigger", 13, "boom", 4);
        CHECK(rc == SPIKEDB_OK, "split trigger put failed");
        char* val = NULL;
        size_t vlen = 0;
        rc = spike_db_get(db, "split_trigger", 13, &val, &vlen);
        CHECK(rc == SPIKEDB_OK && vlen == 4 && memcmp(val, "boom", 4) == 0,
              "split trigger verify failed");
        spike_db_free(val);
    }

    /* Re-verify all original keys survive the split */
    fails = 0;
    for (int i = 0; i < SPIKEDB_KEYS_PER_PAGE; i++) {
        char k[32], expected[32];
        int kl = sprintf(k, "fill_%d", i);
        int el = sprintf(expected, "val_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        SpikeDB_Status rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d keys lost after page split", fails);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 28: Large record near max size
 *============================================================================*/
static void test_large_record(void) {
    printf("  [28] Large record near max size\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    /* Write a value that's close to SPIKEDB_MAX_RECORD_SIZE */
    size_t big_vlen = SPIKEDB_MAX_RECORD_SIZE - 16;  /* leave room for key */
    char* big_val = (char*)malloc(big_vlen);
    CHECK(big_val, "malloc failed");
    /* Fill with a pattern */
    for (size_t i = 0; i < big_vlen; i++)
        big_val[i] = (char)(i & 0xFF);

    SpikeDB_Status rc = spike_db_put(db, "bigrecord", 9, big_val, big_vlen);
    CHECK(rc == SPIKEDB_OK, "large record put failed");

    char* val = NULL;
    size_t vlen = 0;
    rc = spike_db_get(db, "bigrecord", 9, &val, &vlen);
    CHECK(rc == SPIKEDB_OK && vlen == big_vlen, "large record get failed vlen=%zu", vlen);
    CHECK(memcmp(val, big_val, vlen) == 0, "large record data corrupted");
    spike_db_free(val);
    free(big_val);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 29: Rapid delete/re-insert cycles (freelist stress)
 *============================================================================*/
static void test_delete_reinsert_cycles(void) {
    printf("  [29] Rapid delete/re-insert cycles (5 rounds x 500 keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const int N = 500;
    const int ROUNDS = 5;

    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < N; i++)
            put_u64(db, (uint64_t)i, (uint64_t)(i + r * 10000));

        int fails = 0;
        uint64_t val;
        for (int i = 0; i < N; i++) {
            SpikeDB_Status rc = get_u64(db, (uint64_t)i, &val);
            CHECK_CONTINUE(rc == SPIKEDB_OK && val == (uint64_t)(i + r * 10000), fails,
                            "round=%d key=%d rc=%d", r, i, rc);
        }
        if (fails > 0) { CHECK(0, "round %d: %d failures", r, fails); }

        /* Delete all keys */
        for (int i = 0; i < N; i++)
            del_u64(db, (uint64_t)i);

        /* Verify all deleted */
        for (int i = 0; i < N; i++) {
            SpikeDB_Status rc = get_u64(db, (uint64_t)i, &val);
            CHECK_CONTINUE(rc == SPIKEDB_NOT_FOUND, fails,
                            "round=%d key=%d not deleted", r, i);
        }
        if (fails > 0) { CHECK(0, "round %d: %d delete failures", r, fails); }
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 30: Mixed workload benchmark
 *============================================================================*/
static void test_perf_mixed(void) {
    printf("  [30] Perf: mixed workload (50k put + 50k get + 10k delete)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
    CHECK(db, "open failed");

    const int N = 50000;
    SpikeDB_Timer t0, t1;

    /* Phase 1: insert N keys */
    for (int i = 0; i < N; i++)
        put_u64(db, (uint64_t)i, (uint64_t)i * 3);

    /* Phase 2: interleaved get + put + delete */
    uint64_t rng = 0xFEDCBA9876543210ULL;
    int gets = 0, puts = 0, dels = 0, get_hits = 0;

    timer_now(&t0);
    for (int i = 0; i < N; i++) {
        uint64_t r = xorshift64(&rng);
        int op = (int)(r % 10);
        uint64_t key = xorshift64(&rng) % (uint64_t)N;
        uint64_t val;

        if (op < 5) {
            /* 50% get */
            if (get_u64(db, key, &val) == SPIKEDB_OK) get_hits++;
            gets++;
        } else if (op < 8) {
            /* 30% put */
            put_u64(db, key, key + 0xDEAD);
            puts++;
        } else {
            /* 20% delete */
            del_u64(db, key);
            dels++;
        }
    }
    timer_now(&t1);
    double mixed_ms = timer_ms(t0, t1);

    printf("    mixed ops (%d get, %d put, %d del): %.1f ms  (%.0f total ops/s)\n",
           gets, puts, dels, mixed_ms, N / (mixed_ms / 1000.0));
    printf("    get hit rate: %d/%d (%.1f%%)\n", get_hits, gets,
           gets > 0 ? 100.0 * get_hits / gets : 0.0);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * TEST 31: WriteBatch performance comparison
 *============================================================================*/
static void test_perf_writebatch(void) {
    printf("  [31] Perf: WriteBatch vs individual puts (10k keys)\n");
    cleanup();
    const int N = 10000;
    SpikeDB_Timer t0, t1;

    /* Individual puts */
    {
        SpikeDB* db = NULL;
        spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
        CHECK(db, "open failed");

        timer_now(&t0);
        for (int i = 0; i < N; i++) {
            char k[32], v[32];
            int kl = sprintf(k, "key_%d", i);
            int vl = sprintf(v, "val_%d", i);
            spike_db_put(db, k, (size_t)kl, v, (size_t)vl);
        }
        timer_now(&t1);
        double indiv_ms = timer_ms(t0, t1);
        printf("    individual: %.1f ms  (%.0f ops/s)\n", indiv_ms, N / (indiv_ms / 1000.0));

        spike_db_close(db);
        cleanup();
    }

    /* WriteBatch */
    {
        SpikeDB* db = NULL;
        spike_db_open(&db, TEST_DB_PATH, 1, SPIKEDB_OPEN_EXCLUSIVE);
        CHECK(db, "open failed");

        SpikeDB_WriteBatch* batch = spike_db_writebatch_create();
        CHECK(batch, "batch create failed");

        for (int i = 0; i < N; i++) {
            char k[32], v[32];
            int kl = sprintf(k, "key_%d", i);
            int vl = sprintf(v, "val_%d", i);
            spike_db_writebatch_put(batch, k, (size_t)kl, v, (size_t)vl);
        }

        timer_now(&t0);
        spike_db_write(db, batch);
        timer_now(&t1);
        double batch_ms = timer_ms(t0, t1);
        printf("    writebatch: %.1f ms  (%.0f ops/s)\n", batch_ms, N / (batch_ms / 1000.0));

        /* Verify */
        int fails = 0;
        for (int i = 0; i < N; i++) {
            char k[32], expected[32];
            int kl = sprintf(k, "key_%d", i);
            int el = sprintf(expected, "val_%d", i);
            char* val = NULL;
            size_t vlen = 0;
            SpikeDB_Status rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
            if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
                fails++;
            spike_db_free(val);
        }
        CHECK(fails == 0, "%d batch verify failures", fails);

        spike_db_writebatch_destroy(batch);
        spike_db_close(db);
        cleanup();
    }

    PASS();
}

/*============================================================================
 * TEST 32: Long key stress (keys up to 200 bytes)
 *============================================================================*/
static void test_long_keys(void) {
    printf("  [32] Long key stress (200-byte keys)\n");
    cleanup();
    SpikeDB* db = NULL;
    spike_db_open(&db, TEST_DB_PATH, 1, 0);
    CHECK(db, "open failed");

    const int N = 500;
    int fails = 0;

    for (int i = 0; i < N; i++) {
        char k[256], v[64];
        /* Build a long key: "longkey_NNN_padding..." */
        int kl = sprintf(k, "longkey_%d_", i);
        /* Pad to ~200 bytes */
        while (kl < 200) k[kl++] = 'A' + (i % 26);
        int vl = sprintf(v, "value_%d", i);
        SpikeDB_Status rc = spike_db_put(db, k, (size_t)kl, v, (size_t)vl);
        CHECK(rc == SPIKEDB_OK, "long key put %d failed", i);
    }

    for (int i = 0; i < N; i++) {
        char k[256], expected[64];
        int kl = sprintf(k, "longkey_%d_", i);
        while (kl < 200) k[kl++] = 'A' + (i % 26);
        int el = sprintf(expected, "value_%d", i);
        char* val = NULL;
        size_t vlen = 0;
        SpikeDB_Status rc = spike_db_get(db, k, (size_t)kl, &val, &vlen);
        if (rc != SPIKEDB_OK || vlen != (size_t)el || memcmp(val, expected, vlen) != 0)
            fails++;
        spike_db_free(val);
    }
    CHECK(fails == 0, "%d / %d long key checks failed", fails, N);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Main
 *============================================================================*/

int main(void) {
    timer_init();

    printf("======================================================\n");
    printf("  SpikeDB Comprehensive Test Suite\n");
    printf("======================================================\n\n");

    test_basic_crud();
    test_page_boundary();
    test_large_sequential();
    test_reverse_insert();
    test_random_insert();
    test_overwrite_stress();
    test_delete_heavy();
    test_delete_missing();
    test_empty_db();
    test_reopen();
    test_string_keys();
    test_interleaved_insert_delete();
    test_sparse_keys();
    test_same_key_repeat();
    test_delete_all();
    test_reinsert_after_delete();
    test_perf_sequential();
    test_perf_random();
    test_perf_delete();
    test_scale_500k();
    test_writebatch();
    test_binary_data();
    test_empty_key_value();
    test_meta_integrity();
    test_torn_write_recovery();
    test_writebatch_single_commit();
    test_exact_page_fill();
    test_large_record();
    test_delete_reinsert_cycles();
    test_perf_mixed();
    test_perf_writebatch();
    test_long_keys();

    printf("\n======================================================\n");
    printf("  Results: %d passed, %d failed, %d total\n",
           g_tests_passed, g_tests_failed, g_tests_run);
    printf("======================================================\n");

    cleanup();
    return g_tests_failed > 0 ? 1 : 0;
}
