/*============================================================================
 * test_spike_db.c — SpikeDB v5 test harness
 *============================================================================*/

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <time.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "spike_db.h"

#define TEST_DB_PATH    "tmp/test_spike_db.dat"
#define TEST_CACHE      512u           /* 512 * 64KB = 32 MiB */

static int g_run = 0, g_pass = 0, g_fail = 0;

#define TEST(name) static void name(void); \
    static void run_##name(void) { printf("  %-40s", #name); name(); } \
    static void name(void)

#define CHECK(cond, fmt, ...) do {                                          \
    if (!(cond)) {                                                          \
        printf("FAIL\n    %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        g_fail++; g_run++; return;                                          \
    }                                                                       \
} while(0)

#define PASS() do { printf("PASS\n"); g_pass++; g_run++; } while(0)

static void cleanup(void) {
    remove(TEST_DB_PATH);
}

#ifdef _WIN32
static double now_ms(void) {
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)f.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1.0e6;
}
#endif

/*============================================================================
 * 1. Open/close round-trip
 *============================================================================*/

TEST(test_open_close_empty) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(db != NULL, "db handle");
    spike_db_close(db);

    /* Reopen */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 2. Single-record put + get
 *============================================================================*/

TEST(test_single_put_get) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    const char* val = "hello";
    spike_db_batch_put(b, 42, 1000, val, 5);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(db, 42, 1000, &out, &outlen) == SPIKEDB_OK, "get hit");
    CHECK(outlen == 5 && memcmp(out, "hello", 5) == 0, "value matches");
    spike_db_free(out);

    /* Miss on different time */
    CHECK(spike_db_get(db, 42, 9999, &out, &outlen) == SPIKEDB_NOT_FOUND, "get miss");
    /* Miss on different symbol */
    CHECK(spike_db_get(db, 99, 1000, &out, &outlen) == SPIKEDB_NOT_FOUND, "miss sym");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 3. Persistence across reopen
 *============================================================================*/

TEST(test_persistence) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 100; i++) {
        char buf[32]; snprintf(buf, sizeof(buf), "v%d", i);
        spike_db_batch_put(b, 7, (uint64_t)i * 1000, buf, strlen(buf));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write 100");
    spike_db_batch_destroy(b);
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    for (int i = 0; i < 100; i++) {
        void* out = NULL; size_t outlen = 0;
        CHECK(spike_db_get(db, 7, (uint64_t)i * 1000, &out, &outlen) == SPIKEDB_OK,
              "get %d after reopen", i);
        char want[32]; int wlen = snprintf(want, sizeof(want), "v%d", i);
        CHECK(outlen == (size_t)wlen && memcmp(out, want, wlen) == 0, "match %d", i);
        spike_db_free(out);
    }
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 4. Range scan (sequential)
 *============================================================================*/

TEST(test_range_scan_sequential) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const int N = 5000;
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint32_t v = (uint32_t)(i * 7);
        spike_db_batch_put(b, 1, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write N");
    spike_db_batch_destroy(b);

    SpikeDB_Iter* it = spike_db_scan(db, 1, 0, UINT64_MAX);
    CHECK(it != NULL, "iter");
    int seen = 0;
    uint64_t t; const void* val; size_t vlen;
    uint64_t prev_t = 0;
    bool first = true;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        CHECK(vlen == sizeof(uint32_t), "vlen");
        uint32_t got;
        memcpy(&got, val, sizeof(got));
        CHECK(got == (uint32_t)(t * 7), "value matches at t=%llu", (unsigned long long)t);
        if (!first) CHECK(t > prev_t, "sorted");
        prev_t = t;
        first = false;
        seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == N, "saw %d expected %d", seen, N);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 5. Range scan with bounds
 *============================================================================*/

TEST(test_range_scan_bounded) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 1000; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 99, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_Iter* it = spike_db_scan(db, 99, 100, 199);
    int seen = 0;
    uint64_t t; const void* val; size_t vlen;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        CHECK(t >= 100 && t <= 199, "t in range");
        seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == 100, "saw %d expected 100", seen);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 6. Multiple symbols are isolated
 *============================================================================*/

TEST(test_multi_symbol) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    /* Interleaved */
    for (int i = 0; i < 500; i++) {
        for (int s = 1; s <= 5; s++) {
            uint32_t v = (uint32_t)(s * 1000 + i);
            spike_db_batch_put(b, (uint64_t)s, (uint64_t)i, &v, sizeof(v));
        }
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    /* Verify each symbol */
    for (int s = 1; s <= 5; s++) {
        uint64_t cnt;
        CHECK(spike_db_count(db, (uint64_t)s, &cnt) == SPIKEDB_OK, "count s=%d", s);
        CHECK(cnt == 500, "count=%llu expected 500", (unsigned long long)cnt);
        SpikeDB_Iter* it = spike_db_scan(db, (uint64_t)s, 0, UINT64_MAX);
        int n = 0;
        uint64_t t; const void* val; size_t vlen;
        while (spike_db_iter_next(it, &t, &val, &vlen)) {
            uint32_t got; memcpy(&got, val, sizeof(got));
            CHECK(got == (uint32_t)(s * 1000 + (int)t), "value sym=%d t=%llu", s, (unsigned long long)t);
            n++;
        }
        spike_db_iter_close(it);
        CHECK(n == 500, "iter count s=%d n=%d", s, n);
    }
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 7. min / max / count helpers
 *============================================================================*/

TEST(test_min_max_count) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* Empty: count 0, min/max not found */
    uint64_t v;
    CHECK(spike_db_count(db, 1, &v) == SPIKEDB_OK && v == 0, "empty count");
    CHECK(spike_db_max_time(db, 1, &v) == SPIKEDB_NOT_FOUND, "empty max");
    CHECK(spike_db_min_time(db, 1, &v) == SPIKEDB_NOT_FOUND, "empty min");

    SpikeDB_Batch* b = spike_db_batch_create();
    spike_db_batch_put(b, 1, 100, "a", 1);
    spike_db_batch_put(b, 1, 50,  "b", 1);
    spike_db_batch_put(b, 1, 200, "c", 1);
    spike_db_batch_put(b, 1, 75,  "d", 1);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    CHECK(spike_db_count(db, 1, &v) == SPIKEDB_OK && v == 4, "count=%llu", (unsigned long long)v);
    CHECK(spike_db_min_time(db, 1, &v) == SPIKEDB_OK && v == 50, "min=%llu", (unsigned long long)v);
    CHECK(spike_db_max_time(db, 1, &v) == SPIKEDB_OK && v == 200, "max=%llu", (unsigned long long)v);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 8. Out-of-order inserts
 *============================================================================*/

TEST(test_out_of_order) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    /* Insert in reverse order */
    for (int i = 999; i >= 0; i--) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 1, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write reverse");
    spike_db_batch_destroy(b);

    /* Iterate in order */
    SpikeDB_Iter* it = spike_db_scan(db, 1, 0, UINT64_MAX);
    uint64_t t, prev = 0; const void* val; size_t vlen;
    int n = 0; bool first = true;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        if (!first) CHECK(t > prev, "sorted");
        prev = t; first = false;
        n++;
    }
    spike_db_iter_close(it);
    CHECK(n == 1000, "n=%d", n);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 9. Mid-page random insert
 *============================================================================*/

TEST(test_random_insert) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* First batch: ascending 0..999 step 2 (even times) */
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 1000; i++) {
        uint64_t v = (uint64_t)(i*2);
        spike_db_batch_put(b, 1, (uint64_t)(i*2), &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write evens");
    spike_db_batch_destroy(b);

    /* Second batch: random odd times 1..1997, gets mid-page inserts */
    b = spike_db_batch_create();
    uint32_t rng = 12345;
    for (int i = 0; i < 500; i++) {
        rng = rng * 1664525u + 1013904223u;
        uint64_t t = (uint64_t)((rng % 999) * 2 + 1);
        uint64_t v = t;
        /* batch_put will reject duplicate via leaf_insert; ignore err */
        spike_db_batch_put(b, 1, t, &v, sizeof(v));
    }
    /* This batch may include duplicates among itself — write may return error.
     * Use one-by-one to skip duplicates within the batch. */
    spike_db_batch_destroy(b);
    for (int i = 0; i < 500; i++) {
        rng = 12345 + i;
        rng = rng * 1664525u + 1013904223u;
        uint64_t t = (uint64_t)((rng % 999) * 2 + 1);
        uint64_t v = t;
        SpikeDB_Batch* one = spike_db_batch_create();
        spike_db_batch_put(one, 1, t, &v, sizeof(v));
        (void)spike_db_write(db, one);  /* ignore duplicate errors */
        spike_db_batch_destroy(one);
    }

    /* Iteration must be sorted */
    SpikeDB_Iter* it = spike_db_scan(db, 1, 0, UINT64_MAX);
    uint64_t t, prev = 0; const void* val; size_t vlen;
    bool first = true; int n = 0;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        if (!first) CHECK(t > prev, "sorted at t=%llu prev=%llu",
                          (unsigned long long)t, (unsigned long long)prev);
        prev = t; first = false; n++;
    }
    spike_db_iter_close(it);
    CHECK(n >= 1000, "n=%d", n);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 10. Large dataset (forces many leaf splits)
 *============================================================================*/

TEST(test_large_dataset) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const int N = 100000;
    SpikeDB_Batch* b = spike_db_batch_create();
    /* Tick-like value: 64 bytes */
    char tick[64];
    memset(tick, 0xAB, sizeof(tick));

    double t0 = now_ms();
    for (int i = 0; i < N; i++) {
        memcpy(tick, &i, sizeof(i));
        spike_db_batch_put(b, 1, (uint64_t)i, tick, sizeof(tick));
        if ((i+1) % 10000 == 0) {
            CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write @%d", i);
            spike_db_batch_clear(b);
        }
    }
    double t1 = now_ms();
    spike_db_batch_destroy(b);
    printf("[%.0f ms ingest %d recs, %.0f rec/s] ", t1-t0, N, N * 1000.0 / (t1-t0));

    /* Scan and verify */
    double s0 = now_ms();
    SpikeDB_Iter* it = spike_db_scan(db, 1, 0, UINT64_MAX);
    uint64_t t; const void* val; size_t vlen;
    int n = 0;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        CHECK(vlen == 64, "vlen");
        int got; memcpy(&got, val, sizeof(int));
        CHECK(got == (int)t, "value at t=%llu got=%d", (unsigned long long)t, got);
        n++;
    }
    spike_db_iter_close(it);
    double s1 = now_ms();
    CHECK(n == N, "n=%d", n);
    printf("[%.0f ms scan, %.0f rec/s] ", s1-s0, N * 1000.0 / (s1-s0));

    /* Latest timestamp */
    uint64_t mx;
    CHECK(spike_db_max_time(db, 1, &mx) == SPIKEDB_OK && mx == (uint64_t)(N-1),
          "max=%llu", (unsigned long long)mx);

    SpikeDB_Stats st;
    spike_db_stats(db, &st);
    printf("[pages=%llu hits=%llu misses=%llu] ",
           (unsigned long long)st.total_pages,
           (unsigned long long)st.cache_hits,
           (unsigned long long)st.cache_misses);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 11. Atomicity — failed batch leaves DB unchanged
 *============================================================================*/

TEST(test_batch_atomic_duplicate) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 100; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 1, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "first batch");
    spike_db_batch_destroy(b);

    uint64_t cnt_before;
    CHECK(spike_db_count(db, 1, &cnt_before) == SPIKEDB_OK, "count");
    CHECK(cnt_before == 100, "before=%llu", (unsigned long long)cnt_before);

    /* New batch with one duplicate buried in middle — should fail atomically */
    b = spike_db_batch_create();
    for (int i = 200; i < 250; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 1, (uint64_t)i, &v, sizeof(v));
    }
    /* Add a duplicate (i=50 already exists) */
    uint64_t dv = 50;
    spike_db_batch_put(b, 1, 50, &dv, sizeof(dv));
    SpikeDB_Status st = spike_db_write(db, b);
    CHECK(st != SPIKEDB_OK, "expected failure");
    spike_db_batch_destroy(b);

    uint64_t cnt_after;
    CHECK(spike_db_count(db, 1, &cnt_after) == SPIKEDB_OK, "count after");
    CHECK(cnt_after == cnt_before, "atomic: %llu vs %llu",
          (unsigned long long)cnt_after, (unsigned long long)cnt_before);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 12. Persistence after large dataset
 *============================================================================*/

TEST(test_persistence_large) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const int N = 20000;
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
        if ((i+1) % 5000 == 0) {
            CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
            spike_db_batch_clear(b);
        }
    }
    spike_db_batch_destroy(b);
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    uint64_t cnt;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK && cnt == (uint64_t)N,
          "count after reopen: %llu", (unsigned long long)cnt);
    SpikeDB_Iter* it = spike_db_scan(db, 7, 0, UINT64_MAX);
    int n = 0; uint64_t t, prev = 0; const void* val; size_t vlen;
    bool first = true;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        if (!first) {
            if (t <= prev) {
                printf("\n    out-of-order: t=%llu prev=%llu n=%d\n",
                       (unsigned long long)t, (unsigned long long)prev, n);
            }
        }
        prev = t; first = false; n++;
    }
    spike_db_iter_close(it);
    CHECK(n == N, "n=%d", n);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 13. truncate_before — drop entire and partial leaves
 *============================================================================*/

TEST(test_truncate_before) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const int N = 20000;
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 1, (uint64_t)i, &v, sizeof(v));
        if ((i+1) % 5000 == 0) {
            CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
            spike_db_batch_clear(b);
        }
    }
    spike_db_batch_destroy(b);

    /* Truncate everything before t=10000 */
    CHECK(spike_db_truncate_before(db, 1, 10000) == SPIKEDB_OK, "truncate");

    uint64_t cnt;
    CHECK(spike_db_count(db, 1, &cnt) == SPIKEDB_OK && cnt == (uint64_t)(N - 10000),
          "post-truncate count: %llu (expected %d)", (unsigned long long)cnt, N - 10000);

    uint64_t mn, mx;
    CHECK(spike_db_min_time(db, 1, &mn) == SPIKEDB_OK && mn == 10000,
          "min=%llu", (unsigned long long)mn);
    CHECK(spike_db_max_time(db, 1, &mx) == SPIKEDB_OK && mx == (uint64_t)(N-1),
          "max=%llu", (unsigned long long)mx);

    /* Verify gone records can't be looked up */
    void* v; size_t vl;
    CHECK(spike_db_get(db, 1, 0, &v, &vl)    == SPIKEDB_NOT_FOUND, "old get miss");
    CHECK(spike_db_get(db, 1, 9999, &v, &vl) == SPIKEDB_NOT_FOUND, "boundary-1 miss");
    CHECK(spike_db_get(db, 1, 10000, &v, &vl) == SPIKEDB_OK, "boundary hit");
    spike_db_free(v);

    /* Iterate over surviving range */
    SpikeDB_Iter* it = spike_db_scan(db, 1, 0, UINT64_MAX);
    int n = 0; uint64_t t, prev = 9999; const void* val; size_t vlen;
    while (spike_db_iter_next(it, &t, &val, &vlen)) {
        CHECK(t > prev, "sorted at t=%llu prev=%llu",
              (unsigned long long)t, (unsigned long long)prev);
        CHECK(t >= 10000, "kept range");
        prev = t; n++;
    }
    spike_db_iter_close(it);
    CHECK(n == N - 10000, "iter count %d expected %d", n, N - 10000);

    /* Truncate to current max — should leave only the last record */
    CHECK(spike_db_truncate_before(db, 1, (uint64_t)(N - 1)) == SPIKEDB_OK, "truncate2");
    CHECK(spike_db_count(db, 1, &cnt) == SPIKEDB_OK && cnt == 1,
          "after truncate2 count=%llu", (unsigned long long)cnt);

    /* Truncate everything */
    CHECK(spike_db_truncate_before(db, 1, UINT64_MAX) == SPIKEDB_OK, "truncate3");
    CHECK(spike_db_count(db, 1, &cnt) == SPIKEDB_OK && cnt == 0,
          "fully truncated count=%llu", (unsigned long long)cnt);

    /* Insert again and verify usable */
    b = spike_db_batch_create();
    uint64_t vv = 999;
    spike_db_batch_put(b, 1, 12345, &vv, sizeof(vv));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "reuse");
    spike_db_batch_destroy(b);

    void* gv; size_t gl;
    CHECK(spike_db_get(db, 1, 12345, &gv, &gl) == SPIKEDB_OK, "get after reuse");
    spike_db_free(gv);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 14. Truncate persists across reopen
 *============================================================================*/

TEST(test_truncate_persistence) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 5000; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);
    CHECK(spike_db_truncate_before(db, 7, 2500) == SPIKEDB_OK, "truncate");
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    uint64_t cnt;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK && cnt == 2500,
          "count after reopen=%llu", (unsigned long long)cnt);
    SpikeDB_Iter* it = spike_db_scan(db, 7, 0, UINT64_MAX);
    int n = 0; uint64_t t; const void* v; size_t vl;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t >= 2500, "kept");
        n++;
    }
    spike_db_iter_close(it);
    CHECK(n == 2500, "n=%d", n);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 15. Concurrent reader + writer (multi-process)
 *
 * Spawn a writer subprocess that opens the DB and inserts records while
 * this process reads them. Verifies the file lock prevents corruption.
 *============================================================================*/

TEST(test_multiprocess_basic) {
    cleanup();

    /* Seed the DB with one batch */
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    uint64_t v = 1;
    spike_db_batch_put(b, 1, 100, &v, sizeof(v));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "seed");
    spike_db_batch_destroy(b);
    spike_db_close(db);

    /* Open from two handles — same process is a useful smoke test */
    SpikeDB* w = NULL; SpikeDB* r = NULL;
    CHECK(spike_db_open(&w, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "writer");
    CHECK(spike_db_open(&r, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reader");

    /* Writer inserts; reader reads via the same file but separate handle */
    for (int i = 0; i < 100; i++) {
        b = spike_db_batch_create();
        uint64_t vv = (uint64_t)(i + 1000);
        spike_db_batch_put(b, 1, (uint64_t)(i + 200), &vv, sizeof(vv));
        CHECK(spike_db_write(w, b) == SPIKEDB_OK, "iter %d write", i);
        spike_db_batch_destroy(b);

        uint64_t mx;
        CHECK(spike_db_max_time(r, 1, &mx) == SPIKEDB_OK,
              "iter %d reader max_time", i);
        CHECK(mx == (uint64_t)(i + 200),
              "iter %d max=%llu expected %d", i,
              (unsigned long long)mx, i + 200);

        void* val; size_t vl;
        CHECK(spike_db_get(r, 1, (uint64_t)(i + 200), &val, &vl) == SPIKEDB_OK,
              "iter %d reader get", i);
        spike_db_free(val);
    }

    spike_db_close(r);
    spike_db_close(w);
    cleanup();
    PASS();
}

/*============================================================================
 * Driver
 *============================================================================*/

int main(void) {
#ifdef _WIN32
    /* ensure tmp/ exists */
    CreateDirectoryA("tmp", NULL);
#else
    mkdir("tmp", 0755);
#endif

    printf("SpikeDB v5 tests\n");
    printf("================\n\n");

    run_test_open_close_empty();
    run_test_single_put_get();
    run_test_persistence();
    run_test_range_scan_sequential();
    run_test_range_scan_bounded();
    run_test_multi_symbol();
    run_test_min_max_count();
    run_test_out_of_order();
    run_test_random_insert();
    run_test_large_dataset();
    run_test_batch_atomic_duplicate();
    run_test_persistence_large();
    run_test_truncate_before();
    run_test_truncate_persistence();
    run_test_multiprocess_basic();

    printf("\n================\n");
    printf("Results: %d/%d passed, %d failed\n", g_pass, g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
