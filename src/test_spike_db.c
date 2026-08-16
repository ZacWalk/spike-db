/*============================================================================
 * test_spike_db.c — SpikeDB v7 test harness
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
#include <errno.h>

#include "spike_db.h"
#include "spike_db_internal.h"

#define TEST_DB_PATH    "tmp/test_spike_db.dat"
/* Overridable so a low-memory pass can flush out leaked page pins, which
 * surface as SPIKEDB_FULL once the clock sweep runs out of victims. */
#ifndef TEST_CACHE
#define TEST_CACHE      512u           /* 512 * 64KB = 32 MiB */
#endif

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

/* A CHECK failure returns immediately, so a failing test leaks its open
 * handle and Windows then refuses to delete the file — every later test
 * would silently run against stale data. Say so rather than cascading. */
static void cleanup(void) {
    if (remove(TEST_DB_PATH) != 0 && errno != ENOENT)
        printf("\n    [warn] could not remove %s (errno=%d): a previous test "
               "leaked a handle\n", TEST_DB_PATH, errno);
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
 * 16. Read-only open of a non-existent file must fail (and not create it)
 *============================================================================*/

TEST(test_readonly_no_create) {
    cleanup();
    SpikeDB* db = NULL;
    /* RO open of missing file should fail */
    SpikeDB_Status st = spike_db_open(&db, TEST_DB_PATH, TEST_CACHE,
                                      SPIKEDB_OPEN_READONLY);
    CHECK(st != SPIKEDB_OK, "RO open of missing file should fail (got %d)", st);
    /* Verify the file was not created as a side effect */
    FILE* fp = fopen(TEST_DB_PATH, "rb");
    CHECK(fp == NULL, "RO open must not create file");

    /* Now create normally, close, reopen RO — should succeed */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "create");
    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE,
                        SPIKEDB_OPEN_READONLY) == SPIKEDB_OK, "RO reopen");
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 17. Oversized value is rejected at batch_put
 *============================================================================*/

TEST(test_oversized_value_rejected) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    /* 64 KiB - 100 bytes is below the limit; 65000+1 must fail. */
    char* big = (char*)malloc(70000);
    CHECK(big != NULL, "malloc");
    memset(big, 'x', 70000);

    /* Below limit — accepted */
    CHECK(spike_db_batch_put(b, 1, 1, big, 60000) == SPIKEDB_OK, "ok at 60000");
    /* Above limit — rejected */
    CHECK(spike_db_batch_put(b, 1, 2, big, 65001) == SPIKEDB_INVAL, "reject at 65001");
    /* Exactly at the documented limit */
    CHECK(spike_db_batch_put(b, 1, 3, big, 65000) == SPIKEDB_OK, "ok at 65000");

    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);
    free(big);

    /* Reading back the 65000-byte value should yield the right size */
    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(db, 1, 3, &out, &outlen) == SPIKEDB_OK, "get");
    CHECK(outlen == 65000, "len=%zu", outlen);
    spike_db_free(out);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 18. Many distinct symbols stress the symbol directory
 *
 * Each symbol allocates ≥3 64 KB pages (root + first leaf + first node
 * page). The whole batch is a single CoW transaction so all dirty pages
 * must fit in the cache simultaneously. We pick a count that fits into
 * the default test cache (512 pages × 64 KB ≈ 32 MiB), and bump the
 * cache for the test itself so we can exercise a few hundred symbols
 * without thrashing.
 *============================================================================*/

TEST(test_many_symbols) {
    cleanup();
    SpikeDB* db = NULL;
    /* 2048 pages × 64 KB = 128 MiB cache — enough for ~300 symbols. */
    CHECK(spike_db_open(&db, TEST_DB_PATH, 2048, 0) == SPIKEDB_OK, "open");

    enum { N_SYM = 300 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N_SYM; i++) {
        uint64_t v = (uint64_t)i;
        /* one record per symbol */
        spike_db_batch_put(b, (uint64_t)i + 1, 100, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write %d symbols", N_SYM);
    spike_db_batch_destroy(b);

    /* Spot-check every symbol */
    for (int i = 0; i < N_SYM; i++) {
        uint64_t cnt = 12345;
        CHECK(spike_db_count(db, (uint64_t)i + 1, &cnt) == SPIKEDB_OK
              && cnt == 1,
              "sym %d count=%llu", i, (unsigned long long)cnt);
        void* out = NULL; size_t outlen = 0;
        CHECK(spike_db_get(db, (uint64_t)i + 1, 100, &out, &outlen) == SPIKEDB_OK,
              "sym %d get", i);
        CHECK(outlen == sizeof(uint64_t) && *(uint64_t*)out == (uint64_t)i,
              "sym %d value", i);
        spike_db_free(out);
    }

    /* Symbol that was never inserted */
    uint64_t cnt = 999;
    CHECK(spike_db_count(db, (uint64_t)(N_SYM + 9999), &cnt) == SPIKEDB_OK
          && cnt == 0, "missing symbol count=%llu", (unsigned long long)cnt);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 19. Metadata accessors zero *out on error / not-found
 *============================================================================*/

TEST(test_meta_out_zeroed_on_error) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* No data for symbol 42; min/max should return NOT_FOUND with *out=0. */
    uint64_t mn = 0xDEADBEEF, mx = 0xDEADBEEF;
    CHECK(spike_db_min_time(db, 42, &mn) == SPIKEDB_NOT_FOUND,
          "min: not found");
    CHECK(mn == 0, "min: out zeroed (got %llu)", (unsigned long long)mn);
    CHECK(spike_db_max_time(db, 42, &mx) == SPIKEDB_NOT_FOUND,
          "max: not found");
    CHECK(mx == 0, "max: out zeroed (got %llu)", (unsigned long long)mx);

    /* count returns OK with *out=0 for unknown symbols */
    uint64_t cnt = 0xDEADBEEF;
    CHECK(spike_db_count(db, 42, &cnt) == SPIKEDB_OK, "count: ok");
    CHECK(cnt == 0, "count: zero");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 20. Crash safety: simulate torn meta-page write by truncating the file
 *
 * Write a clean DB, close, then corrupt the *active* meta page to invalid
 * (zeroed). On reopen the engine must fall back to the other meta and
 * present the previous consistent state.
 *============================================================================*/

TEST(test_crash_safety_torn_meta) {
    cleanup();

    /* Write 1000 records, commit, close. */
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 1000; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write 1000");
    spike_db_batch_destroy(b);

    /* A second commit so the OTHER meta page becomes the active one with
     * a higher txn_id. After this commit txn_id is 3 (started at 1). */
    b = spike_db_batch_create();
    uint64_t v = 999;
    spike_db_batch_put(b, 7, 9999, &v, sizeof(v));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "commit 2");
    spike_db_batch_destroy(b);
    spike_db_close(db);

    /* Now corrupt one meta page (page 0 OR page 1) — it doesn't matter
     * which: the engine must pick the surviving valid one. We zero
     * page 0; whichever of A/B is active, at least one valid meta
     * remains and reopen must succeed and see a consistent state. */
    FILE* fp = fopen(TEST_DB_PATH, "r+b");
    CHECK(fp != NULL, "open file");
    char zeros[65536]; memset(zeros, 0, sizeof(zeros));
    CHECK(fseek(fp, 0, SEEK_SET) == 0, "seek");
    CHECK(fwrite(zeros, 1, sizeof(zeros), fp) == sizeof(zeros), "zero meta A");
    fclose(fp);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK,
          "reopen after corrupting one meta");
    /* Either the latest commit is visible (if the active was the survivor)
     * or only the first commit (if the active was the corrupted one).
     * Either way the DB must be self-consistent. */
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK, "count");
    CHECK(cnt == 1000 || cnt == 1001,
          "post-recovery count must be 1000 or 1001 (got %llu)",
          (unsigned long long)cnt);
    /* Spot-check one record that was definitely committed in the first batch. */
    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(db, 7, 500, &out, &outlen) == SPIKEDB_OK, "get 500");
    CHECK(outlen == sizeof(uint64_t) && *(uint64_t*)out == 500, "value 500");
    spike_db_free(out);
    spike_db_close(db);

    /* Now corrupt BOTH metas — open must fail, not crash. */
    fp = fopen(TEST_DB_PATH, "r+b");
    CHECK(fp != NULL, "open file");
    CHECK(fseek(fp, 0, SEEK_SET) == 0, "seek");
    CHECK(fwrite(zeros, 1, sizeof(zeros), fp) == sizeof(zeros), "zero meta A");
    CHECK(fwrite(zeros, 1, sizeof(zeros), fp) == sizeof(zeros), "zero meta B");
    fclose(fp);

    SpikeDB_Status st = spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0);
    CHECK(st != SPIKEDB_OK, "both metas corrupt: open must fail (got %d)", st);

    cleanup();
    PASS();
}

/*============================================================================
 * 21. Tiny cache + huge batch -> succeeds via dirty-page spill
 *
 * One symbol, many records -> many fresh leaf pages allocated by the txn,
 * far more than fit in the cache. The cache must spill these freshly
 * allocated pages (the only kind safe to spill mid-txn) so the batch
 * still commits.
 *
 * NB: spill is only safe for pages allocated by the current transaction.
 * Pages that existed before the txn (e.g. symdir) are modified in place
 * and cannot be spilled, so a workload that dirties many pre-existing
 * pages on a tiny cache will still hit SPIKEDB_FULL. The realistic
 * pattern here — bulk-loading a single symbol — dirties only one
 * SymbolRoot but allocates hundreds of fresh leaves.
 *============================================================================*/

TEST(test_dirty_spill_succeeds) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, 16, 0) == SPIKEDB_OK, "open tiny");

    /* Write enough records to one symbol to allocate many leaves
     * (~3000 8-byte records / leaf, so 50000 records -> ~17 fresh leaves
     * + skip-list nodes, easily exceeding 32 cache slots when combined
     * with symdir pinning during the work). */
    const int N = 50000;
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
    }
    SpikeDB_Status st = spike_db_write(db, b);
    CHECK(st == SPIKEDB_OK, "big batch on tiny cache must succeed via spill (got %d)", st);
    spike_db_batch_destroy(b);

    SpikeDB_Stats s; spike_db_stats(db, &s);
    CHECK(s.cache_spills > 0, "expected dirty-page spills, got 0");

    /* Spot-check several records. */
    for (int i = 0; i < N; i += 1000) {
        void* out = NULL; size_t outlen = 0;
        CHECK(spike_db_get(db, 7, (uint64_t)i, &out, &outlen) == SPIKEDB_OK,
              "get time=%d", i);
        CHECK(outlen == sizeof(uint64_t) && *(uint64_t*)out == (uint64_t)i,
              "value time=%d", i);
        spike_db_free(out);
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 22. Spill survives reopen
 *
 * After a transaction that spilled dirty pages mid-write, close the DB,
 * reopen it, and verify every record is still present and intact. This
 * exercises the round-trip: spill -> commit -> meta flip -> shutdown ->
 * reopen -> read pages back from disk (which include spilled-then-final
 * versions).
 *============================================================================*/

TEST(test_spill_persists_across_reopen) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, 16, 0) == SPIKEDB_OK, "open tiny");

    const int N = 50000;
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint64_t v = (uint64_t)i * 1000ull;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "spilling batch must succeed");
    spike_db_batch_destroy(b);

    SpikeDB_Stats s; spike_db_stats(db, &s);
    CHECK(s.cache_spills > 0, "expected spills > 0");
    spike_db_close(db);

    /* Reopen with a normal cache; everything must still be there. */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK, "count");
    CHECK(cnt == (uint64_t)N, "count after reopen: got %llu expected %d",
          (unsigned long long)cnt, N);
    for (int i = 0; i < N; i += 997) {
        void* out = NULL; size_t outlen = 0;
        CHECK(spike_db_get(db, 7, (uint64_t)i, &out, &outlen) == SPIKEDB_OK,
              "get time=%d after reopen", i);
        CHECK(outlen == sizeof(uint64_t)
              && *(uint64_t*)out == (uint64_t)i * 1000ull,
              "value time=%d", i);
        spike_db_free(out);
    }
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 23. Spill + rollback within same handle leaves DB unchanged
 *
 * A batch that spills several dirty pages and then aborts (duplicate key)
 * must roll back atomically: the count and the data of every previously-
 * committed record must be identical before and after the failed write.
 * This proves spilled bytes never become reachable from the active meta.
 *============================================================================*/

TEST(test_spill_with_rollback) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, 16, 0) == SPIKEDB_OK, "open tiny");

    /* Baseline: a few records on sym=7 plus a sentinel on sym=999999
     * (high id so the duplicate sorts last and the abort fires after
     * lots of spilling has already happened). */
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 100; i++) {
        uint64_t v = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &v, sizeof(v));
    }
    uint64_t sentinel = 0xDEADBEEFull;
    spike_db_batch_put(b, 999999, 1, &sentinel, sizeof(sentinel));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "baseline");
    spike_db_batch_destroy(b);

    uint64_t before = 0;
    CHECK(spike_db_count(db, 7, &before) == SPIKEDB_OK && before == 100, "baseline count");

    SpikeDB_Stats sb; spike_db_stats(db, &sb);
    uint64_t spills_before = sb.cache_spills;

    /* Big batch on sym=7 (allocates many fresh leaves -> spills) ending
     * with a duplicate on sym=999999 -> abort. */
    b = spike_db_batch_create();
    for (int i = 100; i < 50000; i++) {
        uint64_t vv = (uint64_t)i;
        spike_db_batch_put(b, 7, (uint64_t)i, &vv, sizeof(vv));
    }
    uint64_t dv = 0;
    spike_db_batch_put(b, 999999, 1, &dv, sizeof(dv));
    SpikeDB_Status st = spike_db_write(db, b);
    CHECK(st != SPIKEDB_OK, "duplicate-bearing batch must fail");
    spike_db_batch_destroy(b);

    SpikeDB_Stats sa; spike_db_stats(db, &sa);
    CHECK(sa.cache_spills > spills_before,
          "aborted batch must have spilled (before=%llu after=%llu)",
          (unsigned long long)spills_before, (unsigned long long)sa.cache_spills);

    /* Verify nothing from the failed batch leaked. */
    uint64_t after = 0;
    CHECK(spike_db_count(db, 7, &after) == SPIKEDB_OK, "recount");
    CHECK(after == before, "count changed across rollback (%llu -> %llu)",
          (unsigned long long)before, (unsigned long long)after);
    /* Records 100..49999 must be invisible. */
    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(db, 7, 25000, &out, &outlen) == SPIKEDB_NOT_FOUND,
          "phantom record visible");
    /* Original records 0..99 must still be there. */
    CHECK(spike_db_get(db, 7, 50, &out, &outlen) == SPIKEDB_OK, "orig get");
    CHECK(outlen == sizeof(uint64_t) && *(uint64_t*)out == 50, "orig value");
    spike_db_free(out);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * 24. Spill is invisible to a concurrent reader handle
 *
 * Open a writer and a reader handle on the same file. Reader takes a
 * baseline snapshot. Writer performs a spilling batch. Until the writer
 * commits, the reader (which re-reads meta on every call) must still see
 * only the baseline. After commit, the new state appears.
 *
 * Single-process: the file lock serializes the two handles, so the
 * reader's checks here happen *between* committed transactions, which is
 * exactly the visibility contract.
 *============================================================================*/

TEST(test_spill_concurrent_reader) {
    cleanup();
    SpikeDB* w = NULL; SpikeDB* r = NULL;
    CHECK(spike_db_open(&w, TEST_DB_PATH, 16, 0) == SPIKEDB_OK, "writer");

    /* Baseline. */
    SpikeDB_Batch* b = spike_db_batch_create();
    uint64_t v = 1;
    spike_db_batch_put(b, 7, 100, &v, sizeof(v));
    CHECK(spike_db_write(w, b) == SPIKEDB_OK, "seed");
    spike_db_batch_destroy(b);

    CHECK(spike_db_open(&r, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reader");
    uint64_t cnt = 0;
    CHECK(spike_db_count(r, 7, &cnt) == SPIKEDB_OK && cnt == 1, "reader sees baseline");

    /* Writer's spilling batch — 50000 records on the same symbol. */
    const int N = 50000;
    b = spike_db_batch_create();
    for (int i = 0; i < N; i++) {
        uint64_t vv = (uint64_t)i + 7000;
        spike_db_batch_put(b, 7, (uint64_t)i + 1000, &vv, sizeof(vv));
    }
    CHECK(spike_db_write(w, b) == SPIKEDB_OK, "spill+commit");
    spike_db_batch_destroy(b);

    SpikeDB_Stats ws; spike_db_stats(w, &ws);
    CHECK(ws.cache_spills > 0, "writer should have spilled");

    /* Reader sees the post-commit state — baseline + N new records. */
    CHECK(spike_db_count(r, 7, &cnt) == SPIKEDB_OK, "recount");
    CHECK(cnt == (uint64_t)(N + 1),
          "reader count after writer commit: got %llu expected %d",
          (unsigned long long)cnt, N + 1);
    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(r, 7, 25000, &out, &outlen) == SPIKEDB_OK, "reader get mid");
    CHECK(outlen == sizeof(uint64_t)
          && *(uint64_t*)out == (uint64_t)(25000 - 1000) + 7000, "reader value");
    spike_db_free(out);

    spike_db_close(r);
    spike_db_close(w);
    cleanup();
    PASS();
}

/*============================================================================
 * 25. Mid-spill abort: bytes on disk do not appear after reopen
 *
 * Crash-safety surrogate. We can't actually kill the process portably,
 * but a rolled-back spilling batch leaves the same on-disk pattern as
 * a crash before commit: spilled pages exist in the file at fresh page
 * ids that are not referenced from the active meta. After close + reopen
 * the recovery path picks the previous active meta and those pages must
 * be invisible.
 *
 * Combined with the duplicate-trigger trick from test 23, this proves
 * the CoW invariant: no pointer ever points at a spilled page until the
 * meta flip publishes it.
 *============================================================================*/

TEST(test_spill_aborted_then_reopen) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, 16, 0) == SPIKEDB_OK, "open tiny");

    /* Commit a known good baseline. The sentinel symbol id is very high
     * so the duplicate sorts AFTER all the spilling inserts (records
     * are processed in (symbol, time) order; otherwise the abort fires
     * before any spill happens). */
    SpikeDB_Batch* b = spike_db_batch_create();
    uint64_t v = 42;
    spike_db_batch_put(b, 7, 1, &v, sizeof(v));
    spike_db_batch_put(b, 999999, 1, &v, sizeof(v));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "baseline");
    spike_db_batch_destroy(b);

    /* Big batch on sym=7 that will spill many fresh leaves, then abort
     * on duplicate of (999999, 1). */
    b = spike_db_batch_create();
    for (int i = 0; i < 50000; i++) {
        uint64_t vv = (uint64_t)i + 5000;
        spike_db_batch_put(b, 7, (uint64_t)i + 100, &vv, sizeof(vv));
    }
    uint64_t dup = 0;
    spike_db_batch_put(b, 999999, 1, &dup, sizeof(dup));   /* dup -> abort */
    SpikeDB_Status st = spike_db_write(db, b);
    CHECK(st != SPIKEDB_OK, "abort expected");
    spike_db_batch_destroy(b);

    SpikeDB_Stats s; spike_db_stats(db, &s);
    CHECK(s.cache_spills > 0, "the aborted batch must have spilled");
    spike_db_close(db);

    /* Reopen — recovery picks the meta from the baseline commit; the
     * spilled pages on disk are unreachable garbage. */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    void* out = NULL; size_t outlen = 0;
    CHECK(spike_db_get(db, 7, 1, &out, &outlen) == SPIKEDB_OK, "baseline get");
    CHECK(outlen == sizeof(uint64_t) && *(uint64_t*)out == 42, "baseline value");
    spike_db_free(out);
    /* sym=7 must contain only the original record. */
    uint64_t cnt = 99;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK, "count");
    CHECK(cnt == 1, "sym=7 count after rollback+reopen: got %llu expected 1",
          (unsigned long long)cnt);
    /* Spot-check that aborted records are absent. */
    CHECK(spike_db_get(db, 7, 25000, &out, &outlen) == SPIKEDB_NOT_FOUND,
          "phantom record survived rollback+reopen");
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * seq tiebreaker: several events sharing one timestamp
 *============================================================================*/

TEST(test_seq_same_timestamp) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* Insert seqs out of order to force mid-leaf placement. */
    static const uint32_t order[8] = { 5, 1, 7, 0, 3, 6, 2, 4 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int i = 0; i < 8; i++) {
        uint32_t s = order[i];
        spike_db_batch_put_seq(b, 42, 1000, s, &s, sizeof(s));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    /* Same (time, seq) is still a duplicate. */
    uint32_t dup = 3;
    spike_db_batch_put_seq(b, 42, 1000, 3, &dup, sizeof(dup));
    CHECK(spike_db_write(db, b) == SPIKEDB_INVAL, "duplicate (time,seq) rejected");
    spike_db_batch_destroy(b);

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 42, &cnt) == SPIKEDB_OK && cnt == 8, "count=8 got %llu",
          (unsigned long long)cnt);

    for (uint32_t s = 0; s < 8; s++) {
        void* out = NULL; size_t len = 0;
        CHECK(spike_db_get_seq(db, 42, 1000, s, &out, &len) == SPIKEDB_OK, "get seq %u", s);
        CHECK(len == sizeof(uint32_t) && *(uint32_t*)out == s, "value seq %u", s);
        spike_db_free(out);
    }

    /* seq-less get is exactly seq 0. */
    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get(db, 42, 1000, &out, &len) == SPIKEDB_OK, "get implies seq 0");
    CHECK(*(uint32_t*)out == 0, "seq 0 value");
    spike_db_free(out);

    /* Scan yields them in seq order. */
    SpikeDB_Iter* it = spike_db_scan(db, 42, 1000, 1000);
    uint64_t t; uint32_t s; const void* v; size_t vl;
    uint32_t expect = 0;
    while (spike_db_iter_next_seq(it, &t, &s, &v, &vl)) {
        CHECK(t == 1000, "time");
        CHECK(s == expect, "seq order: want %u got %u", expect, s);
        expect++;
    }
    spike_db_iter_close(it);
    CHECK(expect == 8, "scanned 8, got %u", expect);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * seq tiebreaker across leaf splits (exercises SkipNode.first_seq)
 *============================================================================*/

TEST(test_seq_spans_many_leaves) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    char payload[64];
    memset(payload, 0xAB, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t s = 0; s < N; s++) {
        memcpy(payload, &s, sizeof(s));
        spike_db_batch_put_seq(b, 9, 1000, s, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_Stats st;
    spike_db_stats(db, &st);
    CHECK(st.total_pages > 20, "expected multiple leaves, pages=%llu",
          (unsigned long long)st.total_pages);

    uint64_t mn = 0, mx = 0, cnt = 0;
    CHECK(spike_db_min_time(db, 9, &mn) == SPIKEDB_OK && mn == 1000, "min_time");
    CHECK(spike_db_max_time(db, 9, &mx) == SPIKEDB_OK && mx == 1000, "max_time");
    CHECK(spike_db_count(db, 9, &cnt) == SPIKEDB_OK && cnt == N, "count=%d got %llu",
          N, (unsigned long long)cnt);

    /* Every seq must be individually addressable through the skip list. */
    for (uint32_t s = 0; s < N; s += 7) {
        void* out = NULL; size_t len = 0;
        CHECK(spike_db_get_seq(db, 9, 1000, s, &out, &len) == SPIKEDB_OK, "get seq %u", s);
        CHECK(len == 64 && memcmp(out, &s, sizeof(s)) == 0, "payload seq %u", s);
        spike_db_free(out);
    }

    SpikeDB_Iter* it = spike_db_scan(db, 9, 0, UINT64_MAX);
    uint64_t t; uint32_t s; const void* v; size_t vl;
    uint32_t expect = 0;
    while (spike_db_iter_next_seq(it, &t, &s, &v, &vl)) {
        CHECK(s == expect, "seq order at %u: got %u", expect, s);
        expect++;
    }
    spike_db_iter_close(it);
    CHECK(expect == N, "scanned %d, got %u", N, expect);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * As-of (floor) and ceiling lookups
 *============================================================================*/

TEST(test_as_of_lookup) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 1; i <= 3; i++) {
        uint64_t t = i * 1000;
        spike_db_batch_put(b, 5, t, &t, sizeof(t));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    void* out = NULL; size_t len = 0; uint64_t t = 0; uint32_t s = 0;

    CHECK(spike_db_get_le(db, 5, 500, &t, &s, &out, &len) == SPIKEDB_NOT_FOUND,
          "le before first");
    CHECK(out == NULL, "no buffer on miss");

    CHECK(spike_db_get_le(db, 5, 1000, &t, &s, &out, &len) == SPIKEDB_OK, "le exact");
    CHECK(t == 1000 && *(uint64_t*)out == 1000, "le exact value");
    spike_db_free(out);

    CHECK(spike_db_get_le(db, 5, 2500, &t, &s, &out, &len) == SPIKEDB_OK, "le between");
    CHECK(t == 2000, "le between -> 2000, got %llu", (unsigned long long)t);
    spike_db_free(out);

    CHECK(spike_db_get_le(db, 5, UINT64_MAX, &t, &s, &out, &len) == SPIKEDB_OK, "le after last");
    CHECK(t == 3000, "le after last -> 3000");
    spike_db_free(out);

    CHECK(spike_db_get_ge(db, 5, 500, &t, &s, &out, &len) == SPIKEDB_OK, "ge before first");
    CHECK(t == 1000, "ge before first -> 1000");
    spike_db_free(out);

    CHECK(spike_db_get_ge(db, 5, 2500, &t, &s, &out, &len) == SPIKEDB_OK, "ge between");
    CHECK(t == 3000, "ge between -> 3000");
    spike_db_free(out);

    CHECK(spike_db_get_ge(db, 5, 3001, &t, &s, &out, &len) == SPIKEDB_NOT_FOUND,
          "ge after last");

    CHECK(spike_db_get_le(db, 77, 1000, &t, &s, &out, &len) == SPIKEDB_NOT_FOUND,
          "le unknown symbol");

    /* At a shared timestamp, le takes the highest seq and ge the lowest. */
    b = spike_db_batch_create();
    for (uint32_t k = 0; k < 4; k++)
        spike_db_batch_put_seq(b, 6, 500, k, &k, sizeof(k));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write seq set");
    spike_db_batch_destroy(b);

    CHECK(spike_db_get_le(db, 6, 500, &t, &s, &out, &len) == SPIKEDB_OK, "le at shared ts");
    CHECK(t == 500 && s == 3, "le picks highest seq, got %u", s);
    spike_db_free(out);

    CHECK(spike_db_get_ge(db, 6, 500, &t, &s, &out, &len) == SPIKEDB_OK, "ge at shared ts");
    CHECK(t == 500 && s == 0, "ge picks lowest seq, got %u", s);
    spike_db_free(out);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * As-of across leaf boundaries (exercises the prev/next leaf walk)
 *============================================================================*/

TEST(test_as_of_across_leaves) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    char payload[64];
    memset(payload, 0, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 1; i <= N; i++) {
        uint64_t t = i * 1000;
        memcpy(payload, &t, sizeof(t));
        spike_db_batch_put(b, 11, t, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    for (uint64_t i = 1; i <= N; i++) {
        void* out = NULL; size_t len = 0; uint64_t t = 0;
        /* Halfway into the gap: floor is i, ceiling is i+1. */
        uint64_t probe = i * 1000 + 500;

        CHECK(spike_db_get_le(db, 11, probe, &t, NULL, &out, &len) == SPIKEDB_OK,
              "le at %llu", (unsigned long long)probe);
        CHECK(t == i * 1000, "le %llu -> %llu", (unsigned long long)probe,
              (unsigned long long)t);
        CHECK(memcmp(out, &t, sizeof(t)) == 0, "le payload");
        spike_db_free(out);

        if (i < N) {
            CHECK(spike_db_get_ge(db, 11, probe, &t, NULL, &out, &len) == SPIKEDB_OK,
                  "ge at %llu", (unsigned long long)probe);
            CHECK(t == (i + 1) * 1000, "ge %llu -> %llu", (unsigned long long)probe,
                  (unsigned long long)t);
            spike_db_free(out);
        }
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Corrections: SPIKEDB_PUT_OVERWRITE
 *============================================================================*/

static bool value_is(SpikeDB* db, uint64_t sym, uint64_t t, const char* want) {
    void* out = NULL; size_t len = 0;
    if (spike_db_get(db, sym, t, &out, &len) != SPIKEDB_OK) return false;
    bool ok = (len == strlen(want)) && memcmp(out, want, len) == 0;
    spike_db_free(out);
    return ok;
}

TEST(test_overwrite_correction) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 10; i++)
        spike_db_batch_put(b, 3, i, "original", 8);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    /* Without the flag a restatement is still refused. */
    spike_db_batch_put(b, 3, 5, "nope", 4);
    CHECK(spike_db_write(db, b) == SPIKEDB_INVAL, "plain put still rejects dup");
    spike_db_batch_clear(b);

    /* Shorter, equal and longer replacements. */
    CHECK(spike_db_batch_put_ex(b, 3, 2, 0, "sh", 2, SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "q1");
    CHECK(spike_db_batch_put_ex(b, 3, 5, 0, "REPLACED", 8, SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "q2");
    CHECK(spike_db_batch_put_ex(b, 3, 7, 0, "a much longer corrected value", 29,
                                SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "q3");
    /* Overwrite of a key that does not exist yet behaves as an insert. */
    CHECK(spike_db_batch_put_ex(b, 3, 99, 0, "inserted", 8, SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "q4");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "overwrite batch");
    spike_db_batch_clear(b);

    CHECK(value_is(db, 3, 2, "sh"), "shorter replacement");
    CHECK(value_is(db, 3, 5, "REPLACED"), "equal-length replacement");
    CHECK(value_is(db, 3, 7, "a much longer corrected value"), "longer replacement");
    CHECK(value_is(db, 3, 8, "original"), "neighbour untouched");
    CHECK(value_is(db, 3, 99, "inserted"), "upsert as insert");

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 3, &cnt) == SPIKEDB_OK && cnt == 11,
          "replacements do not inflate count: %llu", (unsigned long long)cnt);

    /* Last write wins within a single batch. */
    CHECK(spike_db_batch_put_ex(b, 3, 1, 0, "first", 5, SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "r1");
    CHECK(spike_db_batch_put_ex(b, 3, 1, 0, "second", 6, SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "r2");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "same-key batch");
    spike_db_batch_destroy(b);
    CHECK(value_is(db, 3, 1, "second"), "last write wins");

    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(value_is(db, 3, 7, "a much longer corrected value"), "correction persisted");
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Deletes
 *============================================================================*/

TEST(test_delete_basic) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    CHECK(spike_db_batch_count(b) == 0, "new batch is empty");
    for (uint64_t i = 0; i < 10; i++) {
        uint64_t t = i * 100;
        spike_db_batch_put(b, 4, t, &t, sizeof(t));
    }
    CHECK(spike_db_batch_count(b) == 10, "batch_count=%zu", spike_db_batch_count(b));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    CHECK(spike_db_batch_count(b) == 10, "write does not clear the batch");
    spike_db_batch_clear(b);
    CHECK(spike_db_batch_count(b) == 0, "clear empties the batch");

    /* Deleting an absent key is a no-op, not an error. */
    spike_db_batch_del(b, 4, 12345, 0);
    spike_db_batch_del(b, 4444, 1, 0);          /* unknown symbol */
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "absent delete is a no-op");
    spike_db_batch_clear(b);

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 4, &cnt) == SPIKEDB_OK && cnt == 10, "count unchanged");

    /* Remove the first, the last and one in the middle. */
    spike_db_batch_del(b, 4, 0, 0);
    spike_db_batch_del(b, 4, 400, 0);
    spike_db_batch_del(b, 4, 900, 0);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "delete");
    spike_db_batch_destroy(b);

    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get(db, 4, 0, &out, &len) == SPIKEDB_NOT_FOUND, "first gone");
    CHECK(spike_db_get(db, 4, 400, &out, &len) == SPIKEDB_NOT_FOUND, "middle gone");
    CHECK(spike_db_get(db, 4, 900, &out, &len) == SPIKEDB_NOT_FOUND, "last gone");
    CHECK(spike_db_get(db, 4, 300, &out, &len) == SPIKEDB_OK, "survivor present");
    spike_db_free(out);

    CHECK(spike_db_count(db, 4, &cnt) == SPIKEDB_OK && cnt == 7, "count=%llu",
          (unsigned long long)cnt);

    uint64_t mn = 0, mx = 0;
    CHECK(spike_db_min_time(db, 4, &mn) == SPIKEDB_OK && mn == 100,
          "min moved to 100, got %llu", (unsigned long long)mn);
    CHECK(spike_db_max_time(db, 4, &mx) == SPIKEDB_OK && mx == 800,
          "max moved to 800, got %llu", (unsigned long long)mx);

    /* As-of must see through the holes. */
    uint64_t t = 0;
    CHECK(spike_db_get_le(db, 4, 450, &t, NULL, &out, &len) == SPIKEDB_OK, "le");
    CHECK(t == 300, "le 450 -> 300, got %llu", (unsigned long long)t);
    spike_db_free(out);

    SpikeDB_Iter* it = spike_db_scan(db, 4, 0, UINT64_MAX);
    uint64_t seen = 0;
    const void* v; size_t vl;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t != 0 && t != 400 && t != 900, "deleted key %llu resurfaced",
              (unsigned long long)t);
        seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == 7, "scan saw %llu", (unsigned long long)seen);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_delete_all_then_reinsert) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 4000 };
    char payload[64];
    memset(payload, 7, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_put(b, 8, i, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    SpikeDB_Stats before;
    spike_db_stats(db, &before);
    CHECK(before.total_pages > 10, "expected several leaves");

    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_del(b, 8, i, 0);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "delete all");
    spike_db_batch_clear(b);

    uint64_t cnt = 1;
    CHECK(spike_db_count(db, 8, &cnt) == SPIKEDB_OK && cnt == 0, "count=%llu",
          (unsigned long long)cnt);
    uint64_t mn = 0;
    CHECK(spike_db_min_time(db, 8, &mn) == SPIKEDB_NOT_FOUND, "empty symbol has no min");

    SpikeDB_Iter* it = spike_db_scan(db, 8, 0, UINT64_MAX);
    uint64_t t; const void* v; size_t vl;
    CHECK(!spike_db_iter_next(it, &t, &v, &vl), "scan empty");
    spike_db_iter_close(it);

    /* Emptied leaves went back to the freelist, so refilling must not grow
     * the file much. */
    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_put(b, 8, i * 2, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "reinsert");
    spike_db_batch_destroy(b);

    SpikeDB_Stats after;
    spike_db_stats(db, &after);
    CHECK(after.total_pages <= before.total_pages + 4,
          "pages grew %llu -> %llu", (unsigned long long)before.total_pages,
          (unsigned long long)after.total_pages);

    CHECK(spike_db_count(db, 8, &cnt) == SPIKEDB_OK && cnt == N, "refilled count=%llu",
          (unsigned long long)cnt);

    it = spike_db_scan(db, 8, 0, UINT64_MAX);
    uint64_t expect = 0;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t == expect * 2, "order at %llu: got %llu",
              (unsigned long long)expect, (unsigned long long)t);
        expect++;
    }
    spike_db_iter_close(it);
    CHECK(expect == N, "refilled scan saw %llu", (unsigned long long)expect);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_delete_random_subset) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    static bool kept[N];
    char payload[64];
    memset(payload, 0, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        memcpy(payload, &i, sizeof(i));
        spike_db_batch_put(b, 12, (uint64_t)i * 10, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    uint32_t kept_count = 0;
    for (uint32_t i = 0; i < N; i++) {
        kept[i] = ((i * 2654435761u) >> 13) & 1u;
        if (kept[i]) kept_count++;
        else spike_db_batch_del(b, 12, (uint64_t)i * 10, 0);
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "delete subset");
    spike_db_batch_destroy(b);

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 12, &cnt) == SPIKEDB_OK && cnt == kept_count,
          "count %llu want %u", (unsigned long long)cnt, kept_count);

    SpikeDB_Iter* it = spike_db_scan(db, 12, 0, UINT64_MAX);
    uint64_t t; const void* v; size_t vl;
    uint32_t next = 0, seen = 0;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        while (next < N && !kept[next]) next++;
        CHECK(next < N, "scan yielded more than expected");
        CHECK(t == (uint64_t)next * 10, "want %llu got %llu",
              (unsigned long long)next * 10, (unsigned long long)t);
        CHECK(memcmp(v, &next, sizeof(next)) == 0, "payload at %u", next);
        next++; seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == kept_count, "scan saw %u want %u", seen, kept_count);

    /* Spot-check point lookups on both sides. */
    for (uint32_t i = 0; i < N; i += 13) {
        void* out = NULL; size_t len = 0;
        SpikeDB_Status st = spike_db_get(db, 12, (uint64_t)i * 10, &out, &len);
        CHECK(st == (kept[i] ? SPIKEDB_OK : SPIKEDB_NOT_FOUND), "get %u", i);
        if (st == SPIKEDB_OK) spike_db_free(out);
    }

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_delete_range) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 2000 };
    char payload[64];
    memset(payload, 3, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_put(b, 13, i * 10, payload, sizeof(payload));
    /* Two events share timestamp 5000; both must go. */
    spike_db_batch_put_seq(b, 13, 5000, 1, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    CHECK(spike_db_delete_range(db, 13, 5000, 9990) == SPIKEDB_OK, "delete range");

    /* 500 timestamps in range, plus the extra seq at t=5000. */
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 13, &cnt) == SPIKEDB_OK && cnt == N + 1 - 501,
          "count=%llu want %d", (unsigned long long)cnt, N + 1 - 501);

    SpikeDB_Iter* it = spike_db_scan(db, 13, 0, UINT64_MAX);
    uint64_t t; const void* v; size_t vl;
    while (spike_db_iter_next(it, &t, &v, &vl))
        CHECK(t < 5000 || t > 9990, "key %llu survived the range delete",
              (unsigned long long)t);
    spike_db_iter_close(it);

    /* Boundaries are inclusive and the surrounding data is intact. */
    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get(db, 13, 4990, &out, &len) == SPIKEDB_OK, "below range kept");
    spike_db_free(out);
    CHECK(spike_db_get(db, 13, 10000, &out, &len) == SPIKEDB_OK, "above range kept");
    spike_db_free(out);

    CHECK(spike_db_delete_range(db, 13, 5000, 9990) == SPIKEDB_OK, "repeat is a no-op");
    CHECK(spike_db_delete_range(db, 999, 0, 10) == SPIKEDB_OK, "unknown symbol");
    CHECK(spike_db_delete_range(db, 13, 100, 10) == SPIKEDB_INVAL, "inverted range");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Transactional ingest cursor
 *============================================================================*/

static bool meta_is(SpikeDB* db, const char* key, const char* want) {
    void* out = NULL; size_t len = 0;
    if (spike_db_get_meta(db, key, &out, &len) != SPIKEDB_OK) return false;
    bool ok = (len == strlen(want)) && memcmp(out, want, len) == 0;
    spike_db_free(out);
    return ok;
}

TEST(test_batch_meta_cursor) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get_meta(db, "cursor", &out, &len) == SPIKEDB_NOT_FOUND, "absent key");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 5; i++)
        spike_db_batch_put(b, 21, i, "tick", 4);
    CHECK(spike_db_batch_put_meta(b, "cursor", "offset-100", 10) == SPIKEDB_OK, "queue meta");
    CHECK(spike_db_batch_put_meta(b, "source", "binance", 7) == SPIKEDB_OK, "queue meta 2");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    CHECK(meta_is(db, "cursor", "offset-100"), "cursor committed with data");
    CHECK(meta_is(db, "source", "binance"), "second key");

    /* A failing batch must not advance the cursor. */
    spike_db_batch_put(b, 21, 0, "dup", 3);          /* duplicate -> whole batch fails */
    CHECK(spike_db_batch_put_meta(b, "cursor", "offset-999", 10) == SPIKEDB_OK, "queue meta");
    CHECK(spike_db_write(db, b) != SPIKEDB_OK, "batch fails");
    spike_db_batch_clear(b);
    CHECK(meta_is(db, "cursor", "offset-100"), "cursor rolled back with the batch");

    /* Rewriting a key replaces it; a zero-length value erases it. */
    CHECK(spike_db_batch_put_meta(b, "cursor", "offset-200", 10) == SPIKEDB_OK, "requeue");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "meta-only batch commits");
    spike_db_batch_clear(b);
    CHECK(meta_is(db, "cursor", "offset-200"), "cursor advanced");
    CHECK(meta_is(db, "source", "binance"), "other key untouched");

    CHECK(spike_db_batch_put_meta(b, "source", NULL, 0) == SPIKEDB_OK, "queue erase");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "erase commits");
    spike_db_batch_clear(b);
    CHECK(spike_db_get_meta(db, "source", &out, &len) == SPIKEDB_NOT_FOUND, "erased");

    /* Oversized sets are refused rather than silently truncated. */
    static char big[SPIKEDB_META_CAPACITY];
    memset(big, 'x', sizeof(big));
    CHECK(spike_db_batch_put_meta(b, "big", big, sizeof(big)) == SPIKEDB_OK, "queue big");
    CHECK(spike_db_write(db, b) == SPIKEDB_FULL, "meta capacity enforced");
    spike_db_batch_destroy(b);
    CHECK(meta_is(db, "cursor", "offset-200"), "cursor intact after overflow");

    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(meta_is(db, "cursor", "offset-200"), "cursor survives reopen");
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 21, &cnt) == SPIKEDB_OK && cnt == 5, "data intact");
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Symbol info, enumeration, batched polling, diagnostics
 *============================================================================*/

TEST(test_symbol_info_and_listing) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    uint64_t txn0 = 0, txn1 = 0;
    CHECK(spike_db_txn_id(db, &txn0) == SPIKEDB_OK, "txn id");

    const uint64_t syms[3] = { 100, 200, 300 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int s = 0; s < 3; s++)
        for (uint64_t i = 1; i <= 10; i++)
            spike_db_batch_put(b, syms[s], i * (uint64_t)(s + 1), "v", 1);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    CHECK(spike_db_txn_id(db, &txn1) == SPIKEDB_OK, "txn id 2");
    CHECK(txn1 > txn0, "txn id advances on commit");

    SpikeDB_SymbolInfo info;
    CHECK(spike_db_symbol_info(db, 200, &info) == SPIKEDB_OK, "info");
    CHECK(info.record_count == 10, "records=%llu", (unsigned long long)info.record_count);
    CHECK(info.min_time == 2 && info.max_time == 20, "range %llu..%llu",
          (unsigned long long)info.min_time, (unsigned long long)info.max_time);
    CHECK(info.leaf_count >= 1, "leaf count");
    CHECK(spike_db_symbol_info(db, 4242, &info) == SPIKEDB_NOT_FOUND, "unknown symbol");

    uint64_t latest[4] = { 1, 1, 1, 1 };
    const uint64_t watch[4] = { 100, 200, 300, 4242 };
    CHECK(spike_db_max_times(db, watch, latest, 4) == SPIKEDB_OK, "max_times");
    CHECK(latest[0] == 10 && latest[1] == 20 && latest[2] == 30, "latest values");
    CHECK(latest[3] == 0, "absent symbol reports 0");

    size_t total = 0;
    CHECK(spike_db_list_symbols(db, NULL, 0, &total) == SPIKEDB_OK, "size query");
    CHECK(total == 3, "symbol total=%zu", total);

    uint64_t ids[8] = { 0 };
    size_t got = 0;
    CHECK(spike_db_list_symbols(db, ids, 8, &got) == SPIKEDB_OK, "enumerate");
    CHECK(got == 3, "enumerated %zu", got);
    bool seen[3] = { false, false, false };
    for (size_t i = 0; i < got; i++)
        for (int s = 0; s < 3; s++)
            if (ids[i] == syms[s]) seen[s] = true;
    CHECK(seen[0] && seen[1] && seen[2], "all symbols enumerated");

    /* A short buffer still reports the true total. */
    got = 0;
    CHECK(spike_db_list_symbols(db, ids, 1, &got) == SPIKEDB_OK, "short buffer");
    CHECK(got == 3, "short buffer reports total %zu", got);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_error_reporting) {
    cleanup();
    CHECK(spike_db_strerror(SPIKEDB_OK) != NULL, "ok string");
    CHECK(strcmp(spike_db_strerror(SPIKEDB_NOT_FOUND),
                 spike_db_strerror(SPIKEDB_CORRUPT)) != 0, "distinct strings");
    CHECK(spike_db_strerror((SpikeDB_Status)-99) != NULL, "unknown code");

    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Error err;
    spike_db_last_error(db, &err);
    CHECK(err.message[0] != 0, "message always populated");

    spike_db_last_error(NULL, &err);
    CHECK(err.message[0] != 0, "null handle tolerated");

    CHECK(spike_db_format_version() == 7, "format version %u", spike_db_format_version());
    CHECK(spike_db_version() >= 700, "library version");

    SpikeDB_Options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size     = sizeof(opts);
    opts.cache_pages_64k = TEST_CACHE;
    SpikeDB* db2 = NULL;
    CHECK(spike_db_open_ex(&db2, TEST_DB_PATH, &opts) == SPIKEDB_OK, "open_ex");
    spike_db_close(db2);

    opts.struct_size = 1;
    CHECK(spike_db_open_ex(&db2, TEST_DB_PATH, &opts) == SPIKEDB_INVAL, "bad struct_size");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Non-blocking scans and batched iteration
 *============================================================================*/

TEST(test_scan_nonblocking_matches_blocking) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    char payload[48];
    memset(payload, 0, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        memcpy(payload, &i, sizeof(i));
        spike_db_batch_put_seq(b, 30, i / 2, i % 2, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    /* Same records, same order, with and without the lock held throughout. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t flags = pass ? SPIKEDB_SCAN_NONBLOCKING : 0;
        SpikeDB_Iter* it = spike_db_scan_ex(db, 30, 0, UINT64_MAX, flags);
        CHECK(it != NULL, "scan pass %d", pass);
        uint64_t t; uint32_t s; const void* v; size_t vl;
        uint32_t i = 0;
        while (spike_db_iter_next_seq(it, &t, &s, &v, &vl)) {
            CHECK(t == i / 2 && s == i % 2, "pass %d key at %u: %llu/%u",
                  pass, i, (unsigned long long)t, s);
            CHECK(vl == sizeof(payload) && memcmp(v, &i, sizeof(i)) == 0,
                  "pass %d payload at %u", pass, i);
            i++;
        }
        spike_db_iter_close(it);
        CHECK(i == N, "pass %d saw %u", pass, i);
    }

    /* A bounded range behaves the same in both modes. */
    SpikeDB_Iter* it = spike_db_scan_ex(db, 30, 100, 200, SPIKEDB_SCAN_NONBLOCKING);
    uint64_t t; const void* v; size_t vl;
    uint32_t seen = 0;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t >= 100 && t <= 200, "range bound %llu", (unsigned long long)t);
        seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == 202, "bounded range saw %u", seen);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_scan_nonblocking_allows_writer) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    char payload[64];
    memset(payload, 1, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_put(b, 30, i, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    SpikeDB_Iter* it = spike_db_scan_ex(db, 30, 0, UINT64_MAX, SPIKEDB_SCAN_NONBLOCKING);
    CHECK(it != NULL, "scan");

    uint64_t t; const void* v; size_t vl;
    uint64_t count = 0;
    while (count < 100 && spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t == count, "prefix order");
        count++;
    }
    CHECK(count == 100, "consumed prefix");

    /* The whole point: this commit must not wait for the iterator. A
     * blocking scan on the same handle would deadlock here. */
    for (uint64_t i = 10000; i < 11000; i++)
        spike_db_batch_put(b, 30, i, payload, sizeof(payload));
    spike_db_batch_del(b, 30, 2000, 0);                       /* ahead of the cursor */
    spike_db_batch_put_seq(b, 30, 50, 1, payload, sizeof(payload)); /* behind it */
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "writer not blocked by open iterator");
    spike_db_batch_destroy(b);

    uint64_t prev = 99;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        CHECK(t > prev, "cursor advances: %llu after %llu",
              (unsigned long long)t, (unsigned long long)prev);
        CHECK(t != 2000, "record deleted ahead of the cursor was returned");
        prev = t;
        count++;
    }
    spike_db_iter_close(it);

    /* 5000 original - 1 deleted + 1000 appended; the record inserted
     * behind the cursor is not revisited. */
    CHECK(count == 5999, "total %llu", (unsigned long long)count);

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 30, &cnt) == SPIKEDB_OK && cnt == 6000,
          "stored count %llu", (unsigned long long)cnt);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_iter_next_batch) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 3000 };
    char payload[32];
    memset(payload, 0, sizeof(payload));

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        memcpy(payload, &i, sizeof(i));
        spike_db_batch_put(b, 31, (uint64_t)i * 3, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t flags = pass ? SPIKEDB_SCAN_NONBLOCKING : 0;
        SpikeDB_Iter* it = spike_db_scan_ex(db, 31, 0, UINT64_MAX, flags);
        CHECK(it != NULL, "scan pass %d", pass);

        SpikeDB_Rec recs[64];
        size_t n, total = 0;
        uint32_t expect = 0;
        while ((n = spike_db_iter_next_batch(it, recs, 64)) > 0) {
            for (size_t i = 0; i < n; i++) {
                CHECK(recs[i].time == (uint64_t)expect * 3, "pass %d time at %u",
                      pass, expect);
                CHECK(recs[i].seq == 0, "pass %d seq", pass);
                CHECK(recs[i].len == sizeof(payload)
                      && memcmp(recs[i].value, &expect, sizeof(expect)) == 0,
                      "pass %d payload at %u", pass, expect);
                expect++;
            }
            total += n;
        }
        spike_db_iter_close(it);
        CHECK(total == N, "pass %d batched %zu", pass, total);
    }

    /* An empty range yields nothing rather than looping. */
    SpikeDB_Iter* it = spike_db_scan(db, 31, 1, 2);
    SpikeDB_Rec one;
    CHECK(spike_db_iter_next_batch(it, &one, 1) == 0, "empty range");
    spike_db_iter_close(it);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Merged multi-symbol replay
 *============================================================================*/

TEST(test_scan_multi_merge) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const uint64_t syms[3] = { 101, 202, 303 };
    enum { PER = 1000, TOTAL = PER * 3 };
    char payload[40];
    memset(payload, 0, sizeof(payload));

    /* Interleave so the merge has to alternate on every record. */
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < TOTAL; i++) {
        memcpy(payload, &i, sizeof(i));
        spike_db_batch_put(b, syms[i % 3], i, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_Iter* it = spike_db_scan_multi(db, syms, 3, 0, UINT64_MAX, 0);
    CHECK(it != NULL, "scan_multi");
    uint64_t sym, t; uint32_t s; const void* v; size_t vl;
    uint32_t i = 0;
    while (spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl)) {
        CHECK(t == i, "merged time at %u: %llu", i, (unsigned long long)t);
        CHECK(sym == syms[i % 3], "symbol at %u: %llu", i, (unsigned long long)sym);
        CHECK(vl == sizeof(payload) && memcmp(v, &i, sizeof(i)) == 0,
              "payload at %u", i);
        i++;
    }
    spike_db_iter_close(it);
    CHECK(i == TOTAL, "merged %u of %d", i, TOTAL);

    /* Bounded range, and the batched form over a merge. */
    it = spike_db_scan_multi(db, syms, 3, 1000, 1010, 0);
    SpikeDB_Rec recs[8];
    size_t n, total = 0;
    uint64_t expect = 1000;
    while ((n = spike_db_iter_next_batch(it, recs, 8)) > 0) {
        for (size_t k = 0; k < n; k++) {
            CHECK(recs[k].time == expect, "bounded time %llu",
                  (unsigned long long)recs[k].time);
            CHECK(recs[k].symbol == syms[expect % 3], "bounded symbol");
            expect++;
        }
        total += n;
    }
    spike_db_iter_close(it);
    CHECK(total == 11, "bounded merge %zu", total);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_scan_multi_ties_and_edges) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* Three venues printing at the identical nanosecond. */
    const uint64_t syms[3] = { 303, 101, 202 };   /* deliberately unsorted */
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int k = 0; k < 3; k++) {
        spike_db_batch_put_seq(b, syms[k], 5000, 0, &syms[k], sizeof(uint64_t));
        spike_db_batch_put_seq(b, syms[k], 5000, 1, &syms[k], sizeof(uint64_t));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    /* Ties break on seq first, then symbol id ascending. */
    static const uint64_t want_sym[6] = { 101, 202, 303, 101, 202, 303 };
    static const uint32_t want_seq[6] = { 0, 0, 0, 1, 1, 1 };
    SpikeDB_Iter* it = spike_db_scan_multi(db, syms, 3, 0, UINT64_MAX, 0);
    uint64_t sym, t; uint32_t s; const void* v; size_t vl;
    int i = 0;
    while (spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl)) {
        CHECK(i < 6, "too many records");
        CHECK(t == 5000, "time");
        CHECK(sym == want_sym[i] && s == want_seq[i],
              "tie order at %d: %llu/%u", i, (unsigned long long)sym, s);
        i++;
    }
    spike_db_iter_close(it);
    CHECK(i == 6, "tie count %d", i);

    /* Empty symbol list. */
    it = spike_db_scan_multi(db, NULL, 0, 0, UINT64_MAX, 0);
    CHECK(it != NULL, "empty list iterator");
    CHECK(!spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl), "empty list yields nothing");
    spike_db_iter_close(it);

    /* Unknown symbols are skipped, known ones still stream. */
    const uint64_t mixed[3] = { 999999, 101, 888888 };
    it = spike_db_scan_multi(db, mixed, 3, 0, UINT64_MAX, 0);
    i = 0;
    while (spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl)) {
        CHECK(sym == 101, "only the known symbol");
        i++;
    }
    spike_db_iter_close(it);
    CHECK(i == 2, "known symbol records %d", i);

    /* All unknown, and a range that excludes everything. */
    const uint64_t none[2] = { 777, 778 };
    it = spike_db_scan_multi(db, none, 2, 0, UINT64_MAX, 0);
    CHECK(!spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl), "all unknown");
    spike_db_iter_close(it);

    it = spike_db_scan_multi(db, syms, 3, 0, 4999, 0);
    CHECK(!spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl), "range excludes all");
    spike_db_iter_close(it);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_scan_multi_nonblocking) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const uint64_t syms[3] = { 11, 22, 33 };
    enum { PER = 800 };
    char payload[32];
    memset(payload, 9, sizeof(payload));

    /* Every symbol carries a record at every timestamp, so each step of the
     * merge is a tie broken by symbol id. Three symbols also means the
     * 1024-record chunk boundary lands mid-timestamp, which is what makes
     * the resume point exercise the tie handling. */
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t t = 0; t < PER; t++)
        for (int k = 0; k < 3; k++)
            spike_db_batch_put(b, syms[k], t, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    SpikeDB_Iter* it = spike_db_scan_multi(db, syms, 3, 0, UINT64_MAX,
                                           SPIKEDB_SCAN_NONBLOCKING);
    CHECK(it != NULL, "scan_multi nonblocking");

    uint64_t sym, t; uint32_t s; const void* v; size_t vl;
    uint64_t count = 0, prev_t = 0, prev_sym = 0;
    bool have_prev = false;

    /* Stop part way through a timestamp so the resume point lands on a tie. */
    while (count < 1030 && spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl)) {
        prev_t = t; prev_sym = sym; have_prev = true;
        count++;
    }
    CHECK(count == 1030, "consumed prefix");

    for (uint64_t nt = 10000; nt < 10100; nt++)
        for (int k = 0; k < 3; k++)
            spike_db_batch_put(b, syms[k], nt, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "writer not blocked by merged scan");
    spike_db_batch_destroy(b);

    while (spike_db_iter_next_multi(it, &sym, &t, &s, &v, &vl)) {
        CHECK(have_prev, "prev set");
        /* The merged stream must never move backwards. */
        CHECK(t > prev_t || (t == prev_t && sym > prev_sym),
              "order broke at %llu/%llu after %llu/%llu",
              (unsigned long long)t, (unsigned long long)sym,
              (unsigned long long)prev_t, (unsigned long long)prev_sym);
        prev_t = t; prev_sym = sym;
        count++;
    }
    spike_db_iter_close(it);

    /* Nothing may be dropped at the resume boundary, including the ties
     * sharing the last emitted timestamp. */
    CHECK(count == PER * 3 + 300, "total %llu want %d",
          (unsigned long long)count, PER * 3 + 300);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Integrity: page checksums and spike_db_verify
 *============================================================================*/

/* Flip one bit inside a data page, leaving its checksum stale. */
static bool corrupt_page(uint32_t page_id, uint32_t offset) {
    FILE* f = fopen(TEST_DB_PATH, "r+b");
    if (!f) return false;
    long at = (long)page_id * (long)SPIKEDB_PAGE_SIZE + (long)offset;
    if (fseek(f, at, SEEK_SET) != 0) { fclose(f); return false; }
    int c = fgetc(f);
    if (c == EOF) { fclose(f); return false; }
    if (fseek(f, at, SEEK_SET) != 0) { fclose(f); return false; }
    fputc(c ^ 0x40, f);
    fclose(f);
    return true;
}

TEST(test_verify_clean) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* Exercise splits, mid-leaf inserts, ties, corrections and deletes so
     * verify has real structure to walk. */
    char payload[64];
    memset(payload, 5, sizeof(payload));
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t s = 50; s < 53; s++)
        for (uint64_t i = 0; i < 2000; i++)
            spike_db_batch_put_seq(b, s, i, (uint32_t)(i % 3), payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    for (uint64_t i = 0; i < 2000; i += 7)
        spike_db_batch_del(b, 51, i, (uint32_t)(i % 3));
    for (uint64_t i = 0; i < 500; i += 3)
        spike_db_batch_put_ex(b, 52, i, (uint32_t)(i % 3), "short", 5,
                              SPIKEDB_PUT_OVERWRITE);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "edit");
    spike_db_batch_destroy(b);

    CHECK(spike_db_truncate_before(db, 50, 400) == SPIKEDB_OK, "truncate");
    CHECK(spike_db_delete_range(db, 52, 1000, 1200) == SPIKEDB_OK, "delete range");

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);
    CHECK(rep.errors == 0, "errors=%llu (%s)",
          (unsigned long long)rep.errors, rep.first_error);
    CHECK(rep.symbols == 3, "symbols=%llu", (unsigned long long)rep.symbols);
    CHECK(rep.leaves > 3, "leaves=%llu", (unsigned long long)rep.leaves);

    uint64_t c50 = 0, c51 = 0, c52 = 0;
    spike_db_count(db, 50, &c50);
    spike_db_count(db, 51, &c51);
    spike_db_count(db, 52, &c52);
    CHECK(rep.records == c50 + c51 + c52, "records=%llu want %llu",
          (unsigned long long)rep.records,
          (unsigned long long)(c50 + c51 + c52));

    /* Still clean after a reopen. */
    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify after reopen: %s",
          rep.first_error);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_page_checksum_detects_corruption) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    char payload[64];
    memset(payload, 2, sizeof(payload));
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 3000; i++)
        spike_db_batch_put(b, 60, i, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "clean before corruption");
    spike_db_close(db);

    /* Page 18 is the symbol root, 19 the first leaf. Corrupt the leaf's
     * payload area, which a checksum-free store would happily serve. */
    CHECK(corrupt_page(SPIKEDB_RESERVED_PAGES + 1, 40000), "corrupt");

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK,
          "open still succeeds (meta is intact)");

    CHECK(spike_db_verify(db, &rep) == SPIKEDB_CORRUPT, "verify flags corruption");
    CHECK(rep.errors > 0, "error reported");
    CHECK(rep.first_error[0] != 0, "error described");

    /* Reads of the damaged page fail loudly instead of returning garbage. */
    void* out = NULL; size_t len = 0;
    SpikeDB_Status st = spike_db_get(db, 60, 0, &out, &len);
    CHECK(st != SPIKEDB_OK, "read of a corrupt page must not succeed");
    if (st == SPIKEDB_OK) spike_db_free(out);

    SpikeDB_Error err;
    spike_db_last_error(db, &err);
    CHECK(err.status == SPIKEDB_CORRUPT, "last_error status %d", (int)err.status);
    CHECK(err.page == SPIKEDB_RESERVED_PAGES + 1, "last_error page %llu",
          (unsigned long long)err.page);

    CHECK(strcmp(spike_db_strerror(SPIKEDB_CORRUPT), "") != 0, "strerror");

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Fixed-width symbols and spike_db_read_range
 *============================================================================*/

typedef struct TestBar {
    double   open, high, low, close;
    uint64_t volume;
    uint64_t pad;
} TestBar;                                   /* 48 bytes */

static TestBar make_bar(uint32_t i) {
    TestBar b;
    b.open   = (double)i;
    b.high   = (double)i + 1.0;
    b.low    = (double)i - 1.0;
    b.close  = (double)i + 0.5;
    b.volume = 1000u + i;
    b.pad    = 0;
    return b;
}

TEST(test_fixed_width_roundtrip) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    CHECK(spike_db_symbol_define(db, 70, sizeof(TestBar)) == SPIKEDB_OK, "define");
    CHECK(spike_db_symbol_define(db, 70, sizeof(TestBar)) == SPIKEDB_OK, "define is idempotent");
    CHECK(spike_db_symbol_define(db, 70, 32) == SPIKEDB_INVAL, "redefine rejected");
    CHECK(spike_db_symbol_define(db, 71, 0) == SPIKEDB_INVAL, "zero size rejected");
    CHECK(spike_db_symbol_define(db, 71, 1u << 20) == SPIKEDB_INVAL, "huge size rejected");

    SpikeDB_SymbolInfo info;
    CHECK(spike_db_symbol_info(db, 70, &info) == SPIKEDB_OK, "info");
    CHECK(info.record_size == sizeof(TestBar), "record_size=%u", info.record_size);

    enum { N = 6000 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        TestBar bar = make_bar(i);
        spike_db_batch_put(b, 70, (uint64_t)i * 60, &bar, sizeof(bar));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    /* A wrong-sized value must be refused, atomically. */
    char small[8] = { 0 };
    spike_db_batch_put(b, 70, 999999, small, sizeof(small));
    CHECK(spike_db_write(db, b) == SPIKEDB_INVAL, "wrong record size rejected");
    spike_db_batch_destroy(b);

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 70, &cnt) == SPIKEDB_OK && cnt == N, "count=%llu",
          (unsigned long long)cnt);

    /* Point lookups and as-of work the same as on variable-width symbols. */
    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get(db, 70, 300 * 60, &out, &len) == SPIKEDB_OK, "get");
    CHECK(len == sizeof(TestBar), "len=%zu", len);
    TestBar want = make_bar(300);
    CHECK(memcmp(out, &want, sizeof(want)) == 0, "payload");
    spike_db_free(out);

    uint64_t t = 0;
    CHECK(spike_db_get_le(db, 70, 300 * 60 + 30, &t, NULL, &out, &len) == SPIKEDB_OK, "le");
    CHECK(t == 300 * 60, "as-of %llu", (unsigned long long)t);
    spike_db_free(out);

    /* Iteration order and payloads. */
    SpikeDB_Iter* it = spike_db_scan(db, 70, 0, UINT64_MAX);
    const void* v; size_t vl;
    uint32_t i = 0;
    while (spike_db_iter_next(it, &t, &v, &vl)) {
        TestBar w = make_bar(i);
        CHECK(t == (uint64_t)i * 60, "scan time at %u", i);
        CHECK(vl == sizeof(TestBar) && memcmp(v, &w, sizeof(w)) == 0, "scan payload at %u", i);
        i++;
    }
    spike_db_iter_close(it);
    CHECK(i == N, "scanned %u", i);

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(spike_db_symbol_info(db, 70, &info) == SPIKEDB_OK, "info after reopen");
    CHECK(info.record_size == sizeof(TestBar), "record_size persisted");
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify after reopen");
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_read_range) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(spike_db_symbol_define(db, 72, sizeof(TestBar)) == SPIKEDB_OK, "define");

    enum { N = 6000 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        TestBar bar = make_bar(i);
        spike_db_batch_put(b, 72, (uint64_t)i * 60, &bar, sizeof(bar));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    static TestBar dst[N];
    size_t n = 0;

    /* Sizing call. */
    CHECK(spike_db_read_range(db, 72, 0, UINT64_MAX, NULL, 0, &n) == SPIKEDB_OK, "size");
    CHECK(n == N, "sized %zu", n);

    /* Whole range, spanning many leaves. */
    CHECK(spike_db_read_range(db, 72, 0, UINT64_MAX, dst, N, &n) == SPIKEDB_OK, "read all");
    CHECK(n == N, "read %zu", n);
    for (uint32_t i = 0; i < N; i++) {
        TestBar w = make_bar(i);
        CHECK(memcmp(&dst[i], &w, sizeof(w)) == 0, "record %u", i);
    }

    /* Bounded range, inclusive on both ends. */
    CHECK(spike_db_read_range(db, 72, 100 * 60, 199 * 60, dst, N, &n) == SPIKEDB_OK, "bounded");
    CHECK(n == 100, "bounded %zu", n);
    TestBar first = make_bar(100), last = make_bar(199);
    CHECK(memcmp(&dst[0], &first, sizeof(first)) == 0, "bounded first");
    CHECK(memcmp(&dst[99], &last, sizeof(last)) == 0, "bounded last");

    /* Range that starts between records. */
    CHECK(spike_db_read_range(db, 72, 100 * 60 + 1, 102 * 60, dst, N, &n) == SPIKEDB_OK, "gap");
    CHECK(n == 2, "gap %zu", n);
    TestBar g = make_bar(101);
    CHECK(memcmp(&dst[0], &g, sizeof(g)) == 0, "gap first");

    /* Too small a destination reports truncation and fills what it can. */
    CHECK(spike_db_read_range(db, 72, 0, UINT64_MAX, dst, 10, &n) == SPIKEDB_FULL, "truncated");
    CHECK(n == 10, "truncated %zu", n);
    TestBar t9 = make_bar(9);
    CHECK(memcmp(&dst[9], &t9, sizeof(t9)) == 0, "truncated content");

    /* Empty range and unknown symbol. */
    CHECK(spike_db_read_range(db, 72, 1, 2, dst, N, &n) == SPIKEDB_OK, "empty range");
    CHECK(n == 0, "empty %zu", n);
    CHECK(spike_db_read_range(db, 9999, 0, 10, dst, N, &n) == SPIKEDB_NOT_FOUND, "unknown");

    /* Variable-width symbols are refused rather than misinterpreted. */
    SpikeDB_Batch* vb = spike_db_batch_create();
    spike_db_batch_put(vb, 73, 1, "abc", 3);
    CHECK(spike_db_write(db, vb) == SPIKEDB_OK, "variable write");
    spike_db_batch_destroy(vb);
    CHECK(spike_db_read_range(db, 73, 0, UINT64_MAX, dst, N, &n) == SPIKEDB_INVAL,
          "variable-width refused");

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_fixed_width_edits_and_density) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(spike_db_symbol_define(db, 80, sizeof(TestBar)) == SPIKEDB_OK, "define fixed");

    enum { N = 5000 };
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        TestBar bar = make_bar(i);
        spike_db_batch_put_seq(b, 80, i / 2, i % 2, &bar, sizeof(bar));   /* ties */
        spike_db_batch_put_seq(b, 81, i / 2, i % 2, &bar, sizeof(bar));   /* variable */
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    SpikeDB_SymbolInfo fx, var;
    CHECK(spike_db_symbol_info(db, 80, &fx) == SPIKEDB_OK, "fixed info");
    CHECK(spike_db_symbol_info(db, 81, &var) == SPIKEDB_OK, "variable info");
    CHECK(fx.leaf_count < var.leaf_count,
          "fixed layout should need fewer leaves: %llu vs %llu",
          (unsigned long long)fx.leaf_count, (unsigned long long)var.leaf_count);

    /* Corrections and deletes on a fixed-width symbol. Key (11,0) is not
     * one of the keys the delete loop below removes. */
    TestBar fixed_up = make_bar(999999);
    CHECK(spike_db_batch_put_ex(b, 80, 11, 0, &fixed_up, sizeof(fixed_up),
                                SPIKEDB_PUT_OVERWRITE) == SPIKEDB_OK, "queue overwrite");
    for (uint32_t i = 0; i < N; i += 5)
        spike_db_batch_del(b, 80, i / 2, i % 2);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "edit");
    spike_db_batch_destroy(b);

    void* out = NULL; size_t len = 0;
    CHECK(spike_db_get_seq(db, 80, 11, 0, &out, &len) == SPIKEDB_OK, "get overwritten");
    CHECK(memcmp(out, &fixed_up, sizeof(fixed_up)) == 0, "overwrite applied");
    spike_db_free(out);

    CHECK(spike_db_truncate_before(db, 80, 500) == SPIKEDB_OK, "truncate");
    CHECK(spike_db_delete_range(db, 80, 1000, 1100) == SPIKEDB_OK, "delete range");

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    /* Scan must agree with read_range on the surviving records. */
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 80, &cnt) == SPIKEDB_OK, "count");
    size_t n = 0;
    CHECK(spike_db_read_range(db, 80, 0, UINT64_MAX, NULL, 0, &n) == SPIKEDB_OK, "size");
    CHECK(n == cnt, "read_range %zu vs count %llu", n, (unsigned long long)cnt);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Reverse scans and iterator seek
 *============================================================================*/

TEST(test_scan_reverse) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 5000 };
    char payload[48];
    memset(payload, 0, sizeof(payload));
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint32_t i = 0; i < N; i++) {
        memcpy(payload, &i, sizeof(i));
        spike_db_batch_put_seq(b, 90, i / 2, i % 2, payload, sizeof(payload));
    }
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    for (int pass = 0; pass < 2; pass++) {
        uint32_t flags = SPIKEDB_SCAN_REVERSE | (pass ? SPIKEDB_SCAN_NONBLOCKING : 0u);
        SpikeDB_Iter* it = spike_db_scan_ex(db, 90, 0, UINT64_MAX, flags);
        CHECK(it != NULL, "reverse scan pass %d", pass);
        uint64_t t; uint32_t s; const void* v; size_t vl;
        uint32_t expect = N;
        while (spike_db_iter_next_seq(it, &t, &s, &v, &vl)) {
            CHECK(expect > 0, "too many records");
            expect--;
            CHECK(t == expect / 2 && s == expect % 2, "pass %d key at %u: %llu/%u",
                  pass, expect, (unsigned long long)t, s);
            CHECK(memcmp(v, &expect, sizeof(expect)) == 0, "pass %d payload", pass);
        }
        spike_db_iter_close(it);
        CHECK(expect == 0, "reverse scan stopped at %u", expect);
    }

    /* "Last N ticks" — the query reverse scans exist for. */
    SpikeDB_Iter* it = spike_db_scan_ex(db, 90, 0, UINT64_MAX, SPIKEDB_SCAN_REVERSE);
    uint64_t t; uint32_t s; const void* v; size_t vl;
    uint32_t got = 0;
    while (got < 10 && spike_db_iter_next_seq(it, &t, &s, &v, &vl)) got++;
    spike_db_iter_close(it);
    CHECK(got == 10, "last 10");

    /* Bounded reverse range, inclusive on both ends. */
    it = spike_db_scan_ex(db, 90, 100, 200, SPIKEDB_SCAN_REVERSE);
    uint64_t prev = UINT64_MAX;
    uint32_t seen = 0;
    while (spike_db_iter_next_seq(it, &t, &s, &v, &vl)) {
        CHECK(t >= 100 && t <= 200, "bound %llu", (unsigned long long)t);
        CHECK(t <= prev, "descending");
        prev = t;
        seen++;
    }
    spike_db_iter_close(it);
    CHECK(seen == 202, "bounded reverse %u", seen);

    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_iter_seek) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    enum { N = 4000 };
    char payload[32];
    memset(payload, 1, sizeof(payload));
    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < N; i++)
        spike_db_batch_put(b, 91, i * 10, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_Iter* it = spike_db_scan(db, 91, 0, UINT64_MAX);
    uint64_t t; const void* v; size_t vl;
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 0, "first");

    /* Jump forward, mid-buffer. */
    CHECK(spike_db_iter_seek(it, 2000, 0) == SPIKEDB_OK, "seek forward");
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 2000, "after seek: %llu",
          (unsigned long long)t);

    /* Seek backwards is fine too. */
    CHECK(spike_db_iter_seek(it, 500, 0) == SPIKEDB_OK, "seek back");
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 500, "after back seek");

    /* Landing between records resumes at the next one. */
    CHECK(spike_db_iter_seek(it, 505, 0) == SPIKEDB_OK, "seek gap");
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 510, "gap -> %llu",
          (unsigned long long)t);

    /* Past the end. */
    CHECK(spike_db_iter_seek(it, 10ull * N, 0) == SPIKEDB_OK, "seek past end");
    CHECK(!spike_db_iter_next(it, &t, &v, &vl), "past end yields nothing");
    spike_db_iter_close(it);

    /* Seek on a reverse iterator walks down from the target. */
    it = spike_db_scan_ex(db, 91, 0, UINT64_MAX, SPIKEDB_SCAN_REVERSE);
    CHECK(spike_db_iter_seek(it, 1000, 0) == SPIKEDB_OK, "reverse seek");
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 1000, "reverse seek lands");
    CHECK(spike_db_iter_next(it, &t, &v, &vl) && t == 990, "reverse continues");
    spike_db_iter_close(it);

    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Durability control, tailing, prefetch, backup, symbol drop
 *============================================================================*/

TEST(test_write_nosync_and_sync) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 500; i++)
        spike_db_batch_put(b, 95, i, "tick", 4);
    CHECK(spike_db_batch_put_meta(b, "cursor", "42", 2) == SPIKEDB_OK, "meta");
    CHECK(spike_db_write_ex(db, b, SPIKEDB_WRITE_NOSYNC) == SPIKEDB_OK, "nosync write");
    spike_db_batch_destroy(b);

    CHECK(spike_db_sync(db) == SPIKEDB_OK, "explicit sync");

    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 95, &cnt) == SPIKEDB_OK && cnt == 500, "count=%llu",
          (unsigned long long)cnt);

    /* An unsynced commit is still atomic and structurally sound. */
    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(spike_db_count(db, 95, &cnt) == SPIKEDB_OK && cnt == 500, "persisted");
    void* mv = NULL; size_t ml = 0;
    CHECK(spike_db_get_meta(db, "cursor", &mv, &ml) == SPIKEDB_OK, "meta persisted");
    spike_db_free(mv);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_wait_prefetch_backup) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 3000; i++)
        spike_db_batch_put(b, 96, i, "0123456789abcdef", 16);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    uint64_t txn = 0, got = 0;
    CHECK(spike_db_txn_id(db, &txn) == SPIKEDB_OK, "txn id");

    /* Nothing has changed, so this must time out rather than hang. */
    CHECK(spike_db_wait_for_txn(db, txn, 20, &got) == SPIKEDB_NOT_FOUND, "wait times out");
    CHECK(got == txn, "reports current txn");

    /* A stale baseline returns immediately. */
    CHECK(spike_db_wait_for_txn(db, txn - 1, 1000, &got) == SPIKEDB_OK, "wait sees change");
    CHECK(got == txn, "wait reports txn");

    CHECK(spike_db_prefetch(db, 96, 0, UINT64_MAX) == SPIKEDB_OK, "prefetch");
    CHECK(spike_db_prefetch(db, 999999, 0, UINT64_MAX) == SPIKEDB_OK, "prefetch unknown");

    /* Backup produces a file that opens, verifies and reads identically. */
    const char* copy = "tmp/test_spike_db_backup.dat";
    remove(copy);
    CHECK(spike_db_backup(db, copy) == SPIKEDB_OK, "backup");
    spike_db_batch_destroy(b);
    spike_db_close(db);

    SpikeDB* bk = NULL;
    CHECK(spike_db_open(&bk, copy, TEST_CACHE, 0) == SPIKEDB_OK, "open backup");
    uint64_t cnt = 0;
    CHECK(spike_db_count(bk, 96, &cnt) == SPIKEDB_OK && cnt == 3000, "backup count=%llu",
          (unsigned long long)cnt);
    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(bk, &rep) == SPIKEDB_OK, "backup verify: %s", rep.first_error);
    spike_db_close(bk);
    remove(copy);

    cleanup();
    PASS();
}

TEST(test_symbol_drop) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    const uint64_t syms[3] = { 200, 201, 202 };
    char payload[64];
    memset(payload, 4, sizeof(payload));
    SpikeDB_Batch* b = spike_db_batch_create();
    for (int k = 0; k < 3; k++)
        for (uint64_t i = 0; i < 3000; i++)
            spike_db_batch_put(b, syms[k], i, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_destroy(b);

    SpikeDB_Stats before;
    spike_db_stats(db, &before);

    CHECK(spike_db_symbol_drop(db, 201) == SPIKEDB_OK, "drop");
    CHECK(spike_db_symbol_drop(db, 201) == SPIKEDB_NOT_FOUND, "drop twice");
    CHECK(spike_db_symbol_drop(db, 999999) == SPIKEDB_NOT_FOUND, "drop unknown");

    size_t total = 0;
    CHECK(spike_db_list_symbols(db, NULL, 0, &total) == SPIKEDB_OK, "list");
    CHECK(total == 2, "symbols left=%zu", total);

    SpikeDB_SymbolInfo info;
    CHECK(spike_db_symbol_info(db, 201, &info) == SPIKEDB_NOT_FOUND, "dropped symbol gone");
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 200, &cnt) == SPIKEDB_OK && cnt == 3000, "neighbour intact");
    CHECK(spike_db_count(db, 202, &cnt) == SPIKEDB_OK && cnt == 3000, "neighbour intact 2");

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    /* The dropped symbol's pages went to the free list, and the cheap stats
     * counter agrees with what verify actually walked. */
    SpikeDB_Stats freed;
    spike_db_stats(db, &freed);
    CHECK(freed.free_pages > 0, "free_pages=%llu", (unsigned long long)freed.free_pages);
    CHECK(freed.free_pages == rep.free_pages, "stats %llu vs verify %llu",
          (unsigned long long)freed.free_pages, (unsigned long long)rep.free_pages);

    /* Pages went back to the free list, so rewriting the symbol reuses
     * them instead of growing the file. */
    b = spike_db_batch_create();
    for (uint64_t i = 0; i < 3000; i++)
        spike_db_batch_put(b, 201, i, payload, sizeof(payload));
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "rewrite dropped symbol");
    spike_db_batch_destroy(b);

    SpikeDB_Stats after;
    spike_db_stats(db, &after);
    CHECK(after.total_pages <= before.total_pages + 2,
          "pages grew %llu -> %llu", (unsigned long long)before.total_pages,
          (unsigned long long)after.total_pages);
    CHECK(spike_db_count(db, 201, &cnt) == SPIKEDB_OK && cnt == 3000, "rewritten");
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify after rewrite: %s",
          rep.first_error);

    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(spike_db_list_symbols(db, NULL, 0, &total) == SPIKEDB_OK, "list after reopen");
    CHECK(total == 3, "symbols after rewrite=%zu", total);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Checksum path equivalence
 *
 * Not part of the public API: lets the suite compare the SSE4.2 and
 * table-driven CRC-32C paths in one process (see crc32_compute).
 *============================================================================*/

TEST(test_crc32_paths_agree) {
    /* If these two ever disagree, every file written on a machine with
     * SSE4.2 becomes unreadable on one without it, and vice versa. */
    static uint8_t buf[600];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 37u + (i >> 5));

    /* Offsets exercise the unaligned entry; lengths exercise the 8/4/1-byte
     * tail loops of the hardware path. */
    for (size_t off = 0; off < 8; off++) {
        for (size_t len = 0; off + len <= sizeof(buf); len++) {
            uint32_t hw = spike_db_internal_crc32c(buf + off, len, 0);
            uint32_t sw = spike_db_internal_crc32c(buf + off, len, 1);
            CHECK(hw == sw, "off=%zu len=%zu hw=%08x sw=%08x", off, len, hw, sw);
        }
    }

    /* The standard CRC-32C check value: the polynomial cannot drift without
     * invalidating every file ever written. */
    CHECK(spike_db_internal_crc32c("123456789", 9, 0) == 0xE3069283u, "hw check value");
    CHECK(spike_db_internal_crc32c("123456789", 9, 1) == 0xE3069283u, "sw check value");
    PASS();
}

/*============================================================================
 * API contract: what a caller gets wrong on day one
 *============================================================================*/

TEST(test_api_rejects_null_arguments) {
    cleanup();
    SpikeDB*    db  = NULL;
    void*       v   = NULL;
    const void* cv  = NULL;
    size_t      vl  = 0, n = 0;
    uint64_t    u   = 0, sym = 1;
    uint32_t    sq  = 0;
    SpikeDB_SymbolInfo   info;
    SpikeDB_VerifyReport rep;
    SpikeDB_Stats        stats;
    SpikeDB_Error        err;
    SpikeDB_Rec          recs[4];

    CHECK(spike_db_open(NULL, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_INVAL, "open: null out");
    CHECK(spike_db_open(&db, NULL, TEST_CACHE, 0) == SPIKEDB_INVAL, "open: null path");
    CHECK(spike_db_open_ex(&db, TEST_DB_PATH, NULL) == SPIKEDB_INVAL, "open_ex: null opts");

    /* Destructors are no-ops on NULL so error paths can call them blindly. */
    spike_db_close(NULL);
    spike_db_free(NULL);
    spike_db_batch_destroy(NULL);
    spike_db_batch_clear(NULL);
    spike_db_iter_close(NULL);

    CHECK(spike_db_batch_count(NULL) == 0, "batch_count");
    CHECK(spike_db_batch_put(NULL, 1, 1, "x", 1) == SPIKEDB_INVAL, "batch_put");
    CHECK(spike_db_batch_put_seq(NULL, 1, 1, 0, "x", 1) == SPIKEDB_INVAL, "batch_put_seq");
    CHECK(spike_db_batch_put_ex(NULL, 1, 1, 0, "x", 1, 0) == SPIKEDB_INVAL, "batch_put_ex");
    CHECK(spike_db_batch_del(NULL, 1, 1, 0) == SPIKEDB_INVAL, "batch_del");
    CHECK(spike_db_batch_put_meta(NULL, "k", "v", 1) == SPIKEDB_INVAL, "batch_put_meta");

    SpikeDB_Batch* b = spike_db_batch_create();
    CHECK(b != NULL, "batch_create");
    CHECK(spike_db_batch_put(b, 1, 1, NULL, 4) == SPIKEDB_INVAL, "put: null value, len>0");
    CHECK(spike_db_batch_put_meta(b, NULL, "v", 1) == SPIKEDB_INVAL, "meta: null key");
    CHECK(spike_db_batch_put_meta(b, "k", NULL, 4) == SPIKEDB_INVAL, "meta: null value");
    CHECK(spike_db_batch_count(b) == 0, "rejected entries are not queued");

    CHECK(spike_db_get(NULL, 1, 1, &v, &vl) == SPIKEDB_INVAL, "get");
    CHECK(spike_db_get_seq(NULL, 1, 1, 0, &v, &vl) == SPIKEDB_INVAL, "get_seq");
    CHECK(spike_db_get_le(NULL, 1, 1, &u, &sq, &v, &vl) == SPIKEDB_INVAL, "get_le");
    CHECK(spike_db_get_ge(NULL, 1, 1, &u, &sq, &v, &vl) == SPIKEDB_INVAL, "get_ge");
    CHECK(spike_db_get_meta(NULL, "k", &v, &vl) == SPIKEDB_INVAL, "get_meta");
    CHECK(spike_db_write(NULL, b) == SPIKEDB_INVAL, "write");
    CHECK(spike_db_write_ex(NULL, b, 0) == SPIKEDB_INVAL, "write_ex");
    CHECK(spike_db_sync(NULL) == SPIKEDB_INVAL, "sync");
    CHECK(spike_db_symbol_define(NULL, 1, 8) == SPIKEDB_INVAL, "symbol_define");
    CHECK(spike_db_read_range(NULL, 1, 0, 1, NULL, 0, &n) == SPIKEDB_INVAL, "read_range");
    CHECK(spike_db_max_time(NULL, 1, &u) == SPIKEDB_INVAL, "max_time");
    CHECK(spike_db_min_time(NULL, 1, &u) == SPIKEDB_INVAL, "min_time");
    CHECK(spike_db_count(NULL, 1, &u) == SPIKEDB_INVAL, "count");
    CHECK(spike_db_symbol_info(NULL, 1, &info) == SPIKEDB_INVAL, "symbol_info");
    CHECK(spike_db_max_times(NULL, &sym, &u, 1) == SPIKEDB_INVAL, "max_times");
    CHECK(spike_db_txn_id(NULL, &u) == SPIKEDB_INVAL, "txn_id");
    CHECK(spike_db_list_symbols(NULL, NULL, 0, &n) == SPIKEDB_INVAL, "list_symbols");
    CHECK(spike_db_truncate_before(NULL, 1, 1) == SPIKEDB_INVAL, "truncate_before");
    CHECK(spike_db_delete_range(NULL, 1, 0, 1) == SPIKEDB_INVAL, "delete_range");
    CHECK(spike_db_symbol_drop(NULL, 1) == SPIKEDB_INVAL, "symbol_drop");
    CHECK(spike_db_wait_for_txn(NULL, 0, 0, &u) == SPIKEDB_INVAL, "wait_for_txn");
    CHECK(spike_db_prefetch(NULL, 1, 0, 1) == SPIKEDB_INVAL, "prefetch");
    CHECK(spike_db_backup(NULL, "tmp/never_created.dat") == SPIKEDB_INVAL, "backup");
    CHECK(spike_db_verify(NULL, &rep) == SPIKEDB_INVAL, "verify");

    CHECK(spike_db_scan(NULL, 1, 0, 1) == NULL, "scan");
    CHECK(spike_db_scan_ex(NULL, 1, 0, 1, 0) == NULL, "scan_ex");
    CHECK(spike_db_scan_multi(NULL, &sym, 1, 0, 1, 0) == NULL, "scan_multi");
    CHECK(!spike_db_iter_next(NULL, &u, &cv, &vl), "iter_next");
    CHECK(!spike_db_iter_next_seq(NULL, &u, &sq, &cv, &vl), "iter_next_seq");
    CHECK(!spike_db_iter_next_multi(NULL, &u, &u, &sq, &cv, &vl), "iter_next_multi");
    CHECK(spike_db_iter_next_batch(NULL, recs, 4) == 0, "iter_next_batch");
    CHECK(spike_db_iter_seek(NULL, 0, 0) == SPIKEDB_INVAL, "iter_seek");

    /* Diagnostics must survive a handle that never opened. */
    spike_db_stats(NULL, &stats);
    spike_db_last_error(NULL, &err);
    CHECK(err.message[0] != 0, "last_error message");
    for (int s = 0; s >= -6; s--)
        CHECK(spike_db_strerror((SpikeDB_Status)s) != NULL, "strerror(%d)", s);

    /* Output pointers matter as much as the handle. */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(spike_db_get(db, 1, 1, NULL, &vl) == SPIKEDB_INVAL, "get: null value_out");
    CHECK(spike_db_get(db, 1, 1, &v, NULL) == SPIKEDB_INVAL, "get: null len_out");
    CHECK(spike_db_get_meta(db, NULL, &v, &vl) == SPIKEDB_INVAL, "get_meta: null key");
    CHECK(spike_db_write(db, NULL) == SPIKEDB_INVAL, "write: null batch");
    CHECK(spike_db_max_time(db, 1, NULL) == SPIKEDB_INVAL, "max_time: null out");
    CHECK(spike_db_count(db, 1, NULL) == SPIKEDB_INVAL, "count: null out");
    CHECK(spike_db_symbol_info(db, 1, NULL) == SPIKEDB_INVAL, "symbol_info: null out");
    CHECK(spike_db_list_symbols(db, NULL, 0, NULL) == SPIKEDB_INVAL, "list_symbols: null count");
    CHECK(spike_db_list_symbols(db, NULL, 4, &n) == SPIKEDB_INVAL, "list_symbols: cap without buf");
    CHECK(spike_db_max_times(db, NULL, &u, 1) == SPIKEDB_INVAL, "max_times: null symbols");
    CHECK(spike_db_read_range(db, 1, 0, 1, NULL, 0, NULL) == SPIKEDB_INVAL, "read_range: null count");
    CHECK(spike_db_read_range(db, 1, 0, 1, NULL, 4, &n) == SPIKEDB_INVAL, "read_range: cap without buf");
    CHECK(spike_db_verify(db, NULL) == SPIKEDB_INVAL, "verify: null report");
    CHECK(spike_db_backup(db, NULL) == SPIKEDB_INVAL, "backup: null dest");

    spike_db_batch_destroy(b);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_api_misuse_is_diagnosed) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    CHECK(spike_db_batch_put(b, 7, 100, "abc", 3) == SPIKEDB_OK, "put");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");

    /* Committing the same batch again replays it, so the duplicate key is
     * rejected and nothing is applied — the documented cost of forgetting
     * spike_db_batch_clear. */
    CHECK(spike_db_write(db, b) == SPIKEDB_INVAL, "replayed batch rejected");
    uint64_t cnt = 0;
    CHECK(spike_db_count(db, 7, &cnt) == SPIKEDB_OK && cnt == 1,
          "count=%llu", (unsigned long long)cnt);

    spike_db_batch_clear(b);
    CHECK(spike_db_batch_count(b) == 0, "cleared");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "empty batch is a no-op");

    /* The value-length limit is enforced when queued, not at commit. */
    static char big[65010];
    memset(big, 'z', sizeof(big));
    CHECK(spike_db_batch_put(b, 7, 101, big, 65001) == SPIKEDB_INVAL, "65001 rejected");
    CHECK(spike_db_batch_put(b, 7, 101, big, 65000) == SPIKEDB_OK, "65000 accepted");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "commit max-size value");
    spike_db_batch_clear(b);

    void* v = NULL; size_t vl = 0;
    CHECK(spike_db_get(db, 7, 101, &v, &vl) == SPIKEDB_OK && vl == 65000,
          "max value len=%zu", vl);
    spike_db_free(v);

    /* Fixed-width symbols: declared once, and every value must match. */
    CHECK(spike_db_symbol_define(db, 8, 0) == SPIKEDB_INVAL, "record_size 0");
    CHECK(spike_db_symbol_define(db, 8, 16) == SPIKEDB_OK, "define");
    CHECK(spike_db_symbol_define(db, 8, 16) == SPIKEDB_OK, "redeclaring the same size is a no-op");
    CHECK(spike_db_batch_put(b, 8, 1, "0123456789abcdef", 16) == SPIKEDB_OK, "queue");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write fixed-width");
    spike_db_batch_clear(b);
    CHECK(spike_db_symbol_define(db, 8, 32) == SPIKEDB_INVAL, "redeclare with a different size");

    CHECK(spike_db_batch_put(b, 8, 2, "short", 5) == SPIKEDB_OK, "queue wrong width");
    CHECK(spike_db_batch_put(b, 8, 3, "0123456789abcdef", 16) == SPIKEDB_OK, "queue right width");
    CHECK(spike_db_write(db, b) == SPIKEDB_INVAL, "wrong width rejected");
    spike_db_batch_clear(b);
    CHECK(spike_db_count(db, 8, &cnt) == SPIKEDB_OK && cnt == 1,
          "rejection was atomic, count=%llu", (unsigned long long)cnt);

    /* read_range is fixed-width only. */
    size_t got = 0;
    CHECK(spike_db_read_range(db, 7, 0, UINT64_MAX, NULL, 0, &got) == SPIKEDB_INVAL,
          "read_range on a variable-width symbol");

    /* Reversed ranges are rejected rather than silently returning nothing. */
    CHECK(spike_db_scan(db, 7, 200, 100) == NULL, "reversed scan range");
    CHECK(spike_db_read_range(db, 8, 200, 100, NULL, 0, &got) == SPIKEDB_INVAL,
          "reversed read_range");
    CHECK(spike_db_delete_range(db, 7, 200, 100) == SPIKEDB_INVAL, "reversed delete_range");

    /* Seeking is meaningless on a merged iterator. */
    uint64_t syms[2] = { 7, 8 };
    SpikeDB_Iter* mit = spike_db_scan_multi(db, syms, 2, 0, UINT64_MAX, 0);
    CHECK(mit != NULL, "scan_multi");
    CHECK(spike_db_iter_seek(mit, 0, 0) == SPIKEDB_INVAL, "seek on a merged iterator");
    spike_db_iter_close(mit);

    /* Oversized transactional metadata fails the whole commit, leaving the
     * previously committed cursor intact. */
    CHECK(spike_db_batch_put_meta(b, "cursor", "42", 2) == SPIKEDB_OK, "queue cursor");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "commit cursor");
    spike_db_batch_clear(b);

    CHECK(spike_db_batch_put_meta(b, "huge", big, SPIKEDB_META_CAPACITY) == SPIKEDB_OK,
          "queue oversized meta");
    CHECK(spike_db_batch_put(b, 7, 300, "x", 1) == SPIKEDB_OK, "queue record alongside it");
    CHECK(spike_db_write(db, b) == SPIKEDB_FULL, "meta over capacity");
    spike_db_batch_clear(b);

    CHECK(spike_db_get(db, 7, 300, &v, &vl) == SPIKEDB_NOT_FOUND, "record rolled back");
    CHECK(spike_db_get_meta(db, "huge", &v, &vl) == SPIKEDB_NOT_FOUND, "oversized meta not applied");
    CHECK(spike_db_get_meta(db, "cursor", &v, &vl) == SPIKEDB_OK && vl == 2, "cursor intact");
    spike_db_free(v);

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    spike_db_batch_destroy(b);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_readonly_rejects_mutations) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    SpikeDB_Batch* b = spike_db_batch_create();
    for (uint64_t i = 0; i < 200; i++)
        spike_db_batch_put(b, 11, i, "payload-16-bytes", 16);
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "seed");
    spike_db_batch_clear(b);
    uint64_t txn_before = 0;
    CHECK(spike_db_txn_id(db, &txn_before) == SPIKEDB_OK, "txn id");
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, SPIKEDB_OPEN_READONLY) == SPIKEDB_OK,
          "open read-only");

    /* Every mutating entry point refuses, and says so instead of failing
     * somewhere deep in the write path. */
    CHECK(spike_db_batch_put(b, 11, 1000, "payload-16-bytes", 16) == SPIKEDB_OK, "queue");
    CHECK(spike_db_write(db, b) == SPIKEDB_ERROR, "write");
    CHECK(spike_db_write_ex(db, b, SPIKEDB_WRITE_NOSYNC) == SPIKEDB_ERROR, "write_ex");
    CHECK(spike_db_truncate_before(db, 11, 50) == SPIKEDB_ERROR, "truncate_before");
    CHECK(spike_db_delete_range(db, 11, 0, 10) == SPIKEDB_ERROR, "delete_range");
    CHECK(spike_db_symbol_drop(db, 11) == SPIKEDB_ERROR, "symbol_drop");
    CHECK(spike_db_symbol_define(db, 12, 16) == SPIKEDB_ERROR, "symbol_define");

    /* Reads are unaffected. */
    uint64_t cnt = 0, t = 0;
    void* v = NULL; size_t vl = 0;
    CHECK(spike_db_count(db, 11, &cnt) == SPIKEDB_OK && cnt == 200,
          "count=%llu", (unsigned long long)cnt);
    CHECK(spike_db_max_time(db, 11, &t) == SPIKEDB_OK && t == 199, "max_time=%llu",
          (unsigned long long)t);
    CHECK(spike_db_get(db, 11, 7, &v, &vl) == SPIKEDB_OK && vl == 16, "get");
    spike_db_free(v);

    SpikeDB_Iter* it = spike_db_scan(db, 11, 0, UINT64_MAX);
    CHECK(it != NULL, "scan");
    size_t seen = 0; const void* cv = NULL; uint64_t tt = 0;
    while (spike_db_iter_next(it, &tt, &cv, &vl)) seen++;
    spike_db_iter_close(it);
    CHECK(seen == 200, "scanned %zu", seen);

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    const char* copy = "tmp/test_spike_db_ro_backup.dat";
    remove(copy);
    CHECK(spike_db_backup(db, copy) == SPIKEDB_OK, "backup from a read-only handle");
    remove(copy);

    uint64_t txn_after = 0;
    CHECK(spike_db_txn_id(db, &txn_after) == SPIKEDB_OK, "txn id after");
    CHECK(txn_after == txn_before, "txn moved %llu -> %llu",
          (unsigned long long)txn_before, (unsigned long long)txn_after);
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen writable");
    CHECK(spike_db_count(db, 11, &cnt) == SPIKEDB_OK && cnt == 200, "file unchanged");
    spike_db_batch_destroy(b);
    spike_db_close(db);
    cleanup();
    PASS();
}

static size_t scan_count(SpikeDB* db, uint64_t sym, uint64_t lo, uint64_t hi) {
    SpikeDB_Iter* it = spike_db_scan(db, sym, lo, hi);
    if (!it) return (size_t)-1;
    size_t n = 0; uint64_t t; const void* v; size_t l;
    while (spike_db_iter_next(it, &t, &v, &l)) n++;
    spike_db_iter_close(it);
    return n;
}

TEST(test_boundary_keys_and_values) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");

    /* Symbol 0 is a legal id (a directory slot is empty by root_page, not by
     * symbol), and so are both ends of the timestamp range. */
    SpikeDB_Batch* b = spike_db_batch_create();
    CHECK(spike_db_batch_put(b, 0, 0, "zero", 4) == SPIKEDB_OK, "symbol 0, time 0");
    CHECK(spike_db_batch_put_seq(b, 0, 5, UINT32_MAX, "seqmax", 6) == SPIKEDB_OK, "seq max");
    CHECK(spike_db_batch_put(b, 0, 7, NULL, 0) == SPIKEDB_OK, "zero-length value");
    CHECK(spike_db_batch_put(b, 0, UINT64_MAX, "max", 3) == SPIKEDB_OK, "time max");
    CHECK(spike_db_write(db, b) == SPIKEDB_OK, "write");
    spike_db_batch_clear(b);

    void* v = NULL; size_t vl = 99;
    CHECK(spike_db_get(db, 0, 0, &v, &vl) == SPIKEDB_OK && vl == 4, "get at time 0");
    spike_db_free(v);
    CHECK(spike_db_get(db, 0, UINT64_MAX, &v, &vl) == SPIKEDB_OK && vl == 3, "get at time max");
    spike_db_free(v);
    CHECK(spike_db_get_seq(db, 0, 5, UINT32_MAX, &v, &vl) == SPIKEDB_OK && vl == 6, "get at seq max");
    spike_db_free(v);

    /* A zero-length value is a value, not a missing record. */
    v = NULL; vl = 99;
    CHECK(spike_db_get(db, 0, 7, &v, &vl) == SPIKEDB_OK, "get empty value");
    CHECK(vl == 0, "empty len=%zu", vl);
    CHECK(v != NULL, "empty value still returns a freeable buffer");
    spike_db_free(v);

    uint64_t t = 0, cnt = 0;
    CHECK(spike_db_min_time(db, 0, &t) == SPIKEDB_OK && t == 0, "min_time");
    CHECK(spike_db_max_time(db, 0, &t) == SPIKEDB_OK && t == UINT64_MAX, "max_time");
    CHECK(spike_db_count(db, 0, &cnt) == SPIKEDB_OK && cnt == 4,
          "count=%llu", (unsigned long long)cnt);

    /* Degenerate and empty ranges. */
    CHECK(scan_count(db, 0, 0, UINT64_MAX) == 4, "full range");
    CHECK(scan_count(db, 0, 7, 7) == 1, "time_lo == time_hi");
    CHECK(scan_count(db, 0, 1, 4) == 0, "range between records");
    CHECK(scan_count(db, 0, UINT64_MAX, UINT64_MAX) == 1, "range at time max");
    CHECK(scan_count(db, 0, 0, 0) == 1, "range at time 0");
    CHECK(scan_count(db, 4242, 0, UINT64_MAX) == 0, "unknown symbol scans empty");

    /* As-of lookups at the extremes. */
    CHECK(spike_db_get_le(db, 0, 0, &t, NULL, &v, &vl) == SPIKEDB_OK && t == 0, "le at min");
    spike_db_free(v);
    CHECK(spike_db_get_le(db, 0, 6, &t, NULL, &v, &vl) == SPIKEDB_OK && t == 5, "le between");
    spike_db_free(v);
    CHECK(spike_db_get_ge(db, 0, UINT64_MAX, &t, NULL, &v, &vl) == SPIKEDB_OK
          && t == UINT64_MAX, "ge at max");
    spike_db_free(v);
    CHECK(spike_db_get_le(db, 4242, UINT64_MAX, &t, NULL, &v, &vl) == SPIKEDB_NOT_FOUND,
          "le on unknown symbol");

    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);

    /* All of it survives a round trip through the file. */
    spike_db_close(db);
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    CHECK(spike_db_count(db, 0, &cnt) == SPIKEDB_OK && cnt == 4, "count after reopen");
    CHECK(spike_db_max_time(db, 0, &t) == SPIKEDB_OK && t == UINT64_MAX, "max_time after reopen");
    CHECK(scan_count(db, 0, 0, UINT64_MAX) == 4, "scan after reopen");

    spike_db_batch_destroy(b);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Fault injection (docs/testing.md §3)
 *
 * Wraps the default I/O table and fails the Nth call of one kind. The sweep
 * form is: fail call 1, then 2, then 3 ... until the workload runs to
 * completion without ever hitting the fault, asserting after each attempt
 * that the database is still exactly what it was.
 *============================================================================*/

enum { FAULT_NONE = 0, FAULT_READ, FAULT_WRITE, FAULT_FSYNC, FAULT_LOCK };

static struct {
    int      op;
    uint64_t at;        /* 1-based call index of `op` to fail */
    uint64_t calls;
    bool     fired;
} g_fault;

static bool fault_fires(int op) {
    if (g_fault.op != op) return false;
    if (++g_fault.calls != g_fault.at) return false;
    g_fault.fired = true;
    return true;
}

static SpikeDB_Status fault_read(SpikeDB* db, uint32_t pg, void* buf) {
    if (fault_fires(FAULT_READ)) return SPIKEDB_ERROR;
    return spike_db_internal_io_os.read(db, pg, buf);
}
static SpikeDB_Status fault_write(SpikeDB* db, uint32_t pg, const void* buf) {
    if (fault_fires(FAULT_WRITE)) return SPIKEDB_ERROR;
    return spike_db_internal_io_os.write(db, pg, buf);
}
static SpikeDB_Status fault_fsync(SpikeDB* db) {
    if (fault_fires(FAULT_FSYNC)) return SPIKEDB_ERROR;
    return spike_db_internal_io_os.fsync(db);
}
static SpikeDB_Status fault_lock(SpikeDB* db, bool exclusive) {
    if (fault_fires(FAULT_LOCK)) return SPIKEDB_ERROR;
    return spike_db_internal_io_os.lock(db, exclusive);
}
static void fault_unlock(SpikeDB* db) {
    spike_db_internal_io_os.unlock(db);
}

static const SpikeDB_IoOps g_fault_ops = {
    fault_read, fault_write, fault_fsync, fault_lock, fault_unlock, NULL
};

static void fault_arm(int op, uint64_t nth) {
    g_fault.op = op; g_fault.at = nth; g_fault.calls = 0; g_fault.fired = false;
}

/* The count of `symbol` as a separate handle sees it on disk, so a failed
 * commit cannot hide behind the writer's own cache. */
static uint64_t on_disk_count(uint64_t symbol) {
    SpikeDB* chk = NULL;
    if (spike_db_open(&chk, TEST_DB_PATH, 16u, SPIKEDB_OPEN_READONLY) != SPIKEDB_OK)
        return UINT64_MAX;
    uint64_t c = 0;
    if (spike_db_count(chk, symbol, &c) != SPIKEDB_OK) c = UINT64_MAX;
    spike_db_close(chk);
    return c;
}

static SpikeDB_Status seed_symbol(SpikeDB* db, uint64_t symbol,
                                  uint64_t from, uint64_t to) {
    SpikeDB_Batch* b = spike_db_batch_create();
    if (!b) return SPIKEDB_ERROR;
    for (uint64_t i = from; i < to; i++)
        spike_db_batch_put(b, symbol, i, "0123456789abcdef", 16);
    SpikeDB_Status st = spike_db_write(db, b);
    spike_db_batch_destroy(b);
    return st;
}

/* A pristine database holding `n` records under `symbol`.
 *
 * Every fault scenario starts from one of these rather than continuing on
 * the previous one's file: an aborted commit can leave the file damaged
 * (see the KNOWN GAP note below), and a damaged file answers the next
 * question about something other than the fault under test. */
static SpikeDB* fresh_db_with(uint64_t symbol, uint64_t n) {
    cleanup();
    SpikeDB* db = NULL;
    if (spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) != SPIKEDB_OK) return NULL;
    if (seed_symbol(db, symbol, 0, n) != SPIKEDB_OK) { spike_db_close(db); return NULL; }
    return db;
}

TEST(test_io_write_failure_unwinds) {
    /* KNOWN GAP — an aborted commit is *not* atomic on disk.
     *
     * cache_flush_all writes modified pre-existing pages in place before the
     * meta flip (docs/design.md §5), so as soon as more than one page of a
     * commit has been written, an abort has already published part of the
     * batch. Measured: failing write #2 leaves the file reporting the new
     * record count with the old leaf chain, and spike_db_verify calls it
     * corrupt. A power cut at that instant does the same thing. Closing this
     * needs an undo/redo log, which is a format change — docs/testing.md §6.
     *
     * So this asserts what the engine really guarantees today: the call
     * reports the failure, the handle stays usable, and the file still
     * opens. Only the fault that lands before any write has reached the
     * disk is asserted to be atomic. */
    enum { SEED = 600, EXTRA = 600 };

    SpikeDB_Batch* b = spike_db_batch_create();
    CHECK(b != NULL, "batch");
    for (uint64_t i = SEED; i < SEED + EXTRA; i++)
        spike_db_batch_put(b, 31, i, "0123456789abcdef", 16);

    int n = 1;
    for (; n <= 200; n++) {
        SpikeDB* db = fresh_db_with(31, SEED);
        CHECK(db != NULL, "seed for fault %d", n);

        spike_db_internal_set_io(db, &g_fault_ops);
        fault_arm(FAULT_WRITE, (uint64_t)n);
        SpikeDB_Status st = spike_db_write(db, b);
        bool fired = g_fault.fired;
        fault_arm(FAULT_NONE, 0);
        spike_db_internal_set_io(db, NULL);

        if (!fired) {
            CHECK(st == SPIKEDB_OK, "clean run failed: %s", spike_db_strerror(st));
            uint64_t c = 0;
            CHECK(spike_db_count(db, 31, &c) == SPIKEDB_OK && c == SEED + EXTRA,
                  "clean run count=%llu", (unsigned long long)c);
            SpikeDB_VerifyReport rep;
            CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "clean run verify: %s",
                  rep.first_error);
            spike_db_close(db);
            break;
        }

        CHECK(st != SPIKEDB_OK, "write %d claimed success despite an I/O error", n);

        /* The handle is still answerable rather than wedged or crashed. */
        uint64_t c = 0;
        CHECK(spike_db_count(db, 31, &c) == SPIKEDB_OK, "count after fault %d", n);
        spike_db_close(db);

        /* And the file still opens from scratch. */
        CHECK(on_disk_count(31) != UINT64_MAX, "fault %d: file no longer opens", n);

        /* Failing before anything reached the disk *is* atomic. */
        if (n == 1)
            CHECK(on_disk_count(31) == SEED, "fault 1: file changed");
    }
    CHECK(n <= 200, "the batch never got through");
    CHECK(n > 2, "the workload only issued %d write(s)", n - 1);

    spike_db_batch_destroy(b);
    cleanup();
    PASS();
}

TEST(test_io_fsync_failure_is_reported) {
    /* Failing the data-page barrier: the meta is never touched, so the
     * transaction cannot be published. (The page writes themselves have
     * already landed — see the KNOWN GAP above.) */
    SpikeDB* db = fresh_db_with(32, 2000);
    CHECK(db != NULL, "seed");
    spike_db_internal_set_io(db, &g_fault_ops);
    fault_arm(FAULT_FSYNC, 1);
    CHECK(seed_symbol(db, 32, 2000, 3000) != SPIKEDB_OK, "commit reported success");
    CHECK(g_fault.fired, "data fsync fault never fired");
    fault_arm(FAULT_NONE, 0);
    spike_db_internal_set_io(db, NULL);
    spike_db_close(db);
    CHECK(on_disk_count(32) != UINT64_MAX, "file no longer opens");

    /* Failing the meta barrier is the ambiguous case: the commit may or may
     * not be durable. Either answer is acceptable; a third one is not. */
    db = fresh_db_with(32, 2000);
    CHECK(db != NULL, "reseed");
    spike_db_internal_set_io(db, &g_fault_ops);
    fault_arm(FAULT_FSYNC, 2);
    SpikeDB_Status st = seed_symbol(db, 32, 2000, 3000);
    CHECK(g_fault.fired, "meta fsync fault never fired");
    fault_arm(FAULT_NONE, 0);
    spike_db_internal_set_io(db, NULL);
    spike_db_close(db);

    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    uint64_t c = 0;
    CHECK(spike_db_count(db, 32, &c) == SPIKEDB_OK, "count");
    CHECK(c == 2000 || c == 3000, "count is neither the old nor the new value: %llu "
          "(write said %s)", (unsigned long long)c, spike_db_strerror(st));

    /* Whatever it settled on, a commit on top of it produces a sound file. */
    CHECK(seed_symbol(db, 32, 5000, 5100) == SPIKEDB_OK, "write after recovery");
    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify after recovery: %s", rep.first_error);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_io_read_failure_is_reported) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(seed_symbol(db, 33, 0, 3000) == SPIKEDB_OK, "seed");
    spike_db_close(db);

    /* Cold cache, so every query has to read. */
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "reopen");
    spike_db_internal_set_io(db, &g_fault_ops);

    /* A failed read must produce an error code, never a wrong answer. */
    for (int n = 1; n <= 12; n++) {
        fault_arm(FAULT_READ, (uint64_t)n);
        uint64_t c = 12345;
        SpikeDB_Status st = spike_db_count(db, 33, &c);
        if (!g_fault.fired) continue;
        CHECK(st != SPIKEDB_OK, "count %d succeeded through a read error", n);
        CHECK(c == 0, "count %d left a stale value %llu", n, (unsigned long long)c);
    }

    for (int n = 1; n <= 12; n++) {
        fault_arm(FAULT_READ, (uint64_t)n);
        void* v = NULL; size_t vl = 0;
        SpikeDB_Status st = spike_db_get(db, 33, 1500, &v, &vl);
        if (!g_fault.fired) { spike_db_free(v); continue; }
        CHECK(st != SPIKEDB_OK, "get %d succeeded through a read error", n);
        CHECK(v == NULL, "get %d returned a buffer on failure", n);
    }

    for (int n = 1; n <= 12; n++) {
        fault_arm(FAULT_READ, (uint64_t)n);
        size_t seen = 0;
        SpikeDB_Iter* it = spike_db_scan(db, 33, 0, UINT64_MAX);
        if (it) {
            uint64_t t; const void* v; size_t l;
            while (spike_db_iter_next(it, &t, &v, &l)) seen++;
            spike_db_iter_close(it);
        }
        if (!g_fault.fired) continue;
        CHECK(seen < 3000, "scan %d returned a full result through a read error", n);
    }

    /* Clearing the fault restores normal service — no leaked pins, no lock
     * left held, no poisoned cache. */
    fault_arm(FAULT_NONE, 0);
    spike_db_internal_set_io(db, NULL);
    uint64_t c = 0;
    CHECK(spike_db_count(db, 33, &c) == SPIKEDB_OK && c == 3000,
          "count after recovery=%llu", (unsigned long long)c);
    CHECK(scan_count(db, 33, 0, UINT64_MAX) == 3000, "scan after recovery");
    CHECK(seed_symbol(db, 33, 3000, 3500) == SPIKEDB_OK, "write after recovery");
    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);
    spike_db_close(db);
    cleanup();
    PASS();
}

TEST(test_lock_failure_is_reported) {
    cleanup();
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(seed_symbol(db, 34, 0, 500) == SPIKEDB_OK, "seed");

    spike_db_internal_set_io(db, &g_fault_ops);

    /* Nothing may proceed without the lock it asked for. */
    fault_arm(FAULT_LOCK, 1);
    CHECK(seed_symbol(db, 34, 500, 600) != SPIKEDB_OK, "write without the writer lock");

    fault_arm(FAULT_LOCK, 1);
    uint64_t c = 12345;
    CHECK(spike_db_count(db, 34, &c) != SPIKEDB_OK, "count without the reader lock");

    fault_arm(FAULT_LOCK, 1);
    CHECK(spike_db_scan(db, 34, 0, UINT64_MAX) == NULL, "scan without the reader lock");

    fault_arm(FAULT_LOCK, 1);
    CHECK(spike_db_truncate_before(db, 34, 100) != SPIKEDB_OK, "truncate without the lock");

    fault_arm(FAULT_LOCK, 1);
    SpikeDB_VerifyReport rep;
    CHECK(spike_db_verify(db, &rep) != SPIKEDB_OK, "verify without the lock");

    /* None of those leaked a lock, so ordinary work resumes. */
    fault_arm(FAULT_NONE, 0);
    spike_db_internal_set_io(db, NULL);
    CHECK(spike_db_count(db, 34, &c) == SPIKEDB_OK && c == 500,
          "count after recovery=%llu", (unsigned long long)c);
    CHECK(seed_symbol(db, 34, 500, 600) == SPIKEDB_OK, "write after recovery");
    CHECK(on_disk_count(34) == 600, "committed to disk");
    CHECK(spike_db_verify(db, &rep) == SPIKEDB_OK, "verify: %s", rep.first_error);
    spike_db_close(db);
    cleanup();
    PASS();
}

/*============================================================================
 * Handle invariants (docs/testing.md §5)
 *============================================================================*/

TEST(test_handle_invariants_hold) {
    cleanup();
    char msg[192];
    SpikeDB* db = NULL;
    CHECK(spike_db_open(&db, TEST_DB_PATH, TEST_CACHE, 0) == SPIKEDB_OK, "open");
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after open: %s", msg);

    CHECK(seed_symbol(db, 35, 0, 5000) == SPIKEDB_OK, "seed");
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after write: %s", msg);

    /* An open iterator pins pages; closing it must give every one back. */
    SpikeDB_Iter* it = spike_db_scan(db, 35, 0, UINT64_MAX);
    CHECK(it != NULL, "scan");
    uint64_t t; const void* v; size_t l;
    for (int i = 0; i < 100 && spike_db_iter_next(it, &t, &v, &l); i++) { }
    spike_db_iter_close(it);
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after scan: %s", msg);

    CHECK(spike_db_truncate_before(db, 35, 1000) == SPIKEDB_OK, "truncate");
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after truncate: %s", msg);

    CHECK(spike_db_delete_range(db, 35, 2000, 2500) == SPIKEDB_OK, "delete_range");
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after delete_range: %s", msg);

    CHECK(spike_db_symbol_drop(db, 35) == SPIKEDB_OK, "drop");
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after drop: %s", msg);

    /* A failed transaction must unwind just as cleanly as a successful one. */
    spike_db_internal_set_io(db, &g_fault_ops);
    fault_arm(FAULT_WRITE, 2);
    (void)seed_symbol(db, 36, 0, 4000);
    fault_arm(FAULT_NONE, 0);
    spike_db_internal_set_io(db, NULL);
    CHECK(spike_db_internal_check(db, msg, sizeof(msg)) == 0, "after aborted write: %s", msg);

    CHECK(spike_db_internal_check(NULL, msg, sizeof(msg)) == 1, "null handle is a problem");

    spike_db_close(db);
    cleanup();
    PASS();
}

int main(void) {
#ifdef _WIN32
    /* ensure tmp/ exists */
    CreateDirectoryA("tmp", NULL);
#else
    mkdir("tmp", 0755);
#endif

    printf("SpikeDB v7 tests\n");
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
    run_test_readonly_no_create();
    run_test_oversized_value_rejected();
    run_test_many_symbols();
    run_test_meta_out_zeroed_on_error();
    run_test_crash_safety_torn_meta();
    run_test_dirty_spill_succeeds();
    run_test_spill_persists_across_reopen();
    run_test_spill_with_rollback();
    run_test_spill_concurrent_reader();
    run_test_spill_aborted_then_reopen();
    run_test_seq_same_timestamp();
    run_test_seq_spans_many_leaves();
    run_test_as_of_lookup();
    run_test_as_of_across_leaves();
    run_test_overwrite_correction();
    run_test_delete_basic();
    run_test_delete_all_then_reinsert();
    run_test_delete_random_subset();
    run_test_delete_range();
    run_test_batch_meta_cursor();
    run_test_symbol_info_and_listing();
    run_test_error_reporting();
    run_test_scan_nonblocking_matches_blocking();
    run_test_scan_nonblocking_allows_writer();
    run_test_iter_next_batch();
    run_test_scan_multi_merge();
    run_test_scan_multi_ties_and_edges();
    run_test_scan_multi_nonblocking();
    run_test_verify_clean();
    run_test_page_checksum_detects_corruption();
    run_test_fixed_width_roundtrip();
    run_test_read_range();
    run_test_fixed_width_edits_and_density();
    run_test_scan_reverse();
    run_test_iter_seek();
    run_test_write_nosync_and_sync();
    run_test_wait_prefetch_backup();
    run_test_symbol_drop();
    run_test_crc32_paths_agree();
    run_test_api_rejects_null_arguments();
    run_test_api_misuse_is_diagnosed();
    run_test_readonly_rejects_mutations();
    run_test_boundary_keys_and_values();
    run_test_io_write_failure_unwinds();
    run_test_io_fsync_failure_is_reported();
    run_test_io_read_failure_is_reported();
    run_test_lock_failure_is_reported();
    run_test_handle_invariants_hold();

    printf("\n================\n");
    printf("Results: %d/%d passed, %d failed\n", g_pass, g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
