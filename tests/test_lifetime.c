// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_lifetime.c - lifetime / use-after-free / leak tests
 *****************************************************************************
 * Most of our test infra catches FUNCTIONAL bugs (wrong output). This
 * suite specifically targets LIFETIME bugs:
 *
 *   - Use-after-free (UAF): touching memory after free()
 *   - Double-free: calling free() twice on the same pointer
 *   - Leaks: malloc'd memory that is never freed
 *   - Uninitialized-resource teardown: e.g. sem_destroy on a sem that
 *     sem_init never succeeded on
 *
 * These bugs typically don't change visible output until they crash —
 * sometimes never on a single test run. We rely on AddressSanitizer
 * to flag them. The tests are designed to make the bug-relevant code
 * paths execute many times in patterns that surface incremental leaks
 * (1 byte * 1000 iterations = 1KB shadow trace) and partial-init
 * teardowns (immediate destroy without ever calling apply).
 *
 * To run with full leak detection:
 *   make build/test_lifetime && ASAN_OPTIONS=detect_leaks=1 build/test_lifetime
 *
 * The default `make test` target already builds with ASan + UBSan and
 * runs this suite.
 *****************************************************************************/

#include "../src/usm_pool.h"
#include "../src/usm.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- test framework (same minimal pattern as other suites) ---- */

static int g_run = 0, g_fail = 0, g_failed_in_test = 0;

#define BEGIN(name) do { printf("  [....] %s\n", name); g_failed_in_test = 0; } while (0)
#define END() do { g_run++; if (g_failed_in_test) { g_fail++; } } while (0)
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        g_failed_in_test = 1; \
    } \
} while (0)

/* ---- helpers ---- */

static void fill_deterministic(uint8_t *buf, size_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 0xC0FFEEu;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 24);
    }
}

/* ---- Lifetime test 1: create + immediate destroy (no apply) ----
 *
 * Targets the "destroy a pool that never lazy-init'd" path. With lazy
 * init, this is the most common partial-state case: workers allocated,
 * threads NOT spawned, semaphores NOT initialized. The destroy must
 * walk the partial state without crashing or leaking.
 *
 * UAF risk this catches:
 *   - sem_destroy on uninitialized sem (rare but possible if guard
 *     flag is missing)
 *   - free() of an uninitialized pointer
 *   - join on a thread that was never created
 */
static void test_create_then_destroy_no_apply(void)
{
    BEGIN("create + destroy without apply (cold pool teardown)");
    for (int trial = 0; trial < 50; trial++) {
        usm_pool_t *p = up_usm_pool_create(8, 320, 240);
        CHECK(p != NULL);
        up_usm_pool_destroy(p);
        /* If a UAF happens HERE, ASan flags it on this iter. If a leak
         * happens, ASan's leak detector flags it at process exit. */
    }
    END();
}

/* ---- Lifetime test 2: create + single apply + destroy ----
 *
 * Exercises the lazy-init path: first apply triggers worker thread
 * spawn and workspace alloc. Destroy then has to tear down the FULL
 * initialized state. Done many times to surface any incremental leak
 * in the alloc/free pairing.
 */
static void test_create_apply_destroy(void)
{
    BEGIN("create + apply + destroy (warm pool teardown)");
    int width = 256, height = 144;
    size_t n = (size_t)width * height;
    uint8_t *src = malloc(n);
    uint8_t *dst = malloc(n);
    CHECK(src && dst);

    int amount = up_usm_amount_pct_to_q8(30);
    for (int trial = 0; trial < 30; trial++) {
        usm_pool_t *p = up_usm_pool_create(4, width, height);
        CHECK(p != NULL);
        fill_deterministic(src, n, 0xD00Du + trial);
        memset(dst, 0xAA, n);
        int rc = up_usm_pool_apply(p, dst, width, src, width, amount);
        CHECK(rc == 0);
        up_usm_pool_destroy(p);
    }
    free(src); free(dst);
    END();
}

/* ---- Lifetime test 3: many create-destroy cycles ----
 *
 * Smoke test for the destroy path under repeated stress. A single
 * leak of N bytes per cycle would accumulate to N*1000 bytes by
 * the end, well above ASan's reporting threshold.
 *
 * Varies thread count and dimensions so each cycle exercises a
 * slightly different allocation pattern (workspace size depends
 * on dimensions; worker array size depends on thread count).
 */
static void test_many_cycles(void)
{
    BEGIN("1000 create-destroy cycles (leak surface)");
    for (int i = 0; i < 1000; i++) {
        int n_threads = (i % 16) + 1;     /* 1..16 */
        int width  = 64 + (i % 64) * 4;   /* 64..316 */
        int height = 48 + (i % 32) * 2;   /* 48..110 */
        usm_pool_t *p = up_usm_pool_create(n_threads, width, height);
        CHECK(p != NULL);
        up_usm_pool_destroy(p);
    }
    END();
}

/* ---- Lifetime test 4: alternating cold/warm cycles ----
 *
 * Half the iterations destroy without apply (cold path), half with
 * apply (warm path). Catches lifetime bugs that only manifest when
 * the destroy state varies between iterations — e.g., a static
 * variable or shared resource that wasn't reset properly.
 */
static void test_alternating_cold_warm(void)
{
    BEGIN("alternating cold + warm destroy cycles");
    int width = 128, height = 96;
    size_t n = (size_t)width * height;
    uint8_t *src = malloc(n);
    uint8_t *dst = malloc(n);
    CHECK(src && dst);
    fill_deterministic(src, n, 42);

    int amount = up_usm_amount_pct_to_q8(50);
    for (int i = 0; i < 50; i++) {
        usm_pool_t *p = up_usm_pool_create(4, width, height);
        CHECK(p != NULL);
        if (i % 2 == 0) {
            /* Warm: apply once before destroy */
            up_usm_pool_apply(p, dst, width, src, width, amount);
        }
        /* Cold: just destroy without apply */
        up_usm_pool_destroy(p);
    }
    free(src); free(dst);
    END();
}

/* ---- Lifetime test 5: destroy NULL is idempotent ----
 *
 * Documented contract: destroy(NULL) is a no-op. This is critical for
 * the autoupscale.c Close() path which always calls
 * up_usm_pool_destroy(p_sys->usm_pool) even when create returned NULL.
 */
static void test_destroy_null_repeatedly(void)
{
    BEGIN("destroy(NULL) called many times is safe");
    for (int i = 0; i < 100; i++) {
        up_usm_pool_destroy(NULL);
    }
    END();
}

/* ---- Lifetime test 6: invalid args don't allocate ----
 *
 * Confirms create() with degenerate args returns NULL without
 * allocating anything. If create() ever leaked partial state on
 * the error path, this would surface in ASan's leak report.
 */
static void test_create_invalid_no_leak(void)
{
    BEGIN("create with invalid args allocates nothing");
    const usm_pool_t *p1 = up_usm_pool_create(0, 100, 100);   /* zero threads */
    CHECK(p1 == NULL);
    const usm_pool_t *p2 = up_usm_pool_create(4, 0, 100);     /* zero width */
    CHECK(p2 == NULL);
    const usm_pool_t *p3 = up_usm_pool_create(4, 100, 0);     /* zero height */
    CHECK(p3 == NULL);
    const usm_pool_t *p4 = up_usm_pool_create(-1, 100, 100);  /* negative */
    CHECK(p4 == NULL);
    /* If any of those leaked workers/workspace, ASan reports at exit. */
    END();
}

/* ---- Lifetime test 7: input buffer can be modified after apply ----
 *
 * Sanity check that apply() doesn't retain pointers to the input
 * buffer beyond the call. If a worker cached the src pointer and
 * read it on the next apply (which never happens in our model
 * because workers signal back before apply returns), this test
 * would NOT catch it directly — but it does catch the simpler
 * case of "src is touched after apply returns."
 *
 * The check: free the src buffer right after apply, allocate a new
 * different one with different content, apply again. If the pool
 * silently kept a stale pointer to the freed buffer, ASan would
 * flag use-after-free on the next apply.
 */
static void test_input_buffer_can_be_freed(void)
{
    BEGIN("input buffer can be freed between applies (no pointer caching)");
    int width = 96, height = 72;
    size_t n = (size_t)width * height;
    uint8_t *dst = malloc(n);
    CHECK(dst);

    usm_pool_t *p = up_usm_pool_create(4, width, height);
    CHECK(p != NULL);

    int amount = up_usm_amount_pct_to_q8(30);
    for (int i = 0; i < 10; i++) {
        /* Each iter: alloc src, apply, FREE src. Next iter gets a fresh src
         * at a DIFFERENT address. A pool that retained the old pointer
         * would now read freed memory; ASan flags it. */
        uint8_t *src = malloc(n);
        CHECK(src);
        if (!src) continue;
        fill_deterministic(src, n, 0xA1B2u + i);
        memset(dst, 0xCC, n);
        int rc = up_usm_pool_apply(p, dst, width, src, width, amount);
        CHECK(rc == 0);
        free(src);  /* src is now freed; pool must not retain it */
    }

    up_usm_pool_destroy(p);
    free(dst);
    END();
}

/* ---- Lifetime test 8: dst buffer can be reallocated between applies ----
 *
 * Complement of test 7: confirms the pool doesn't retain pointers to
 * the destination buffer either. We swap dst between two different
 * heap addresses every iteration; both apply calls must write to the
 * CURRENT dst, not a cached old one.
 */
static void test_dst_buffer_swappable(void)
{
    BEGIN("dst buffer can be reallocated between applies (no pointer caching)");
    int width = 80, height = 60;
    size_t n = (size_t)width * height;
    uint8_t *src = malloc(n);
    CHECK(src);
    fill_deterministic(src, n, 7);

    usm_pool_t *p = up_usm_pool_create(2, width, height);
    CHECK(p != NULL);

    int amount = up_usm_amount_pct_to_q8(30);
    for (int i = 0; i < 10; i++) {
        uint8_t *dst = malloc(n);
        CHECK(dst);
        if (!dst) continue;
        memset(dst, 0xDD, n);
        int rc = up_usm_pool_apply(p, dst, width, src, width, amount);
        CHECK(rc == 0);
        /* If pool held a pointer to a previously-passed dst, that dst is
         * now freed below; the next iter's apply or the destroy might
         * touch it. */
        free(dst);
    }

    up_usm_pool_destroy(p);
    free(src);
    END();
}

/* ---- Lifetime test 9: variable-sized pools in succession ----
 *
 * Each pool has a different workspace size. Free + create with
 * different size must not retain or reuse stale state.
 */
static void test_variable_sizes_in_succession(void)
{
    BEGIN("variable-size pool create/destroy in succession");
    int sizes[][2] = {
        { 64,  48 }, { 256, 144 }, { 32,  32 },
        { 854, 480 }, { 16,  16 }, { 1024, 768 },
        { 100, 100 }, { 1280, 720 }, { 80,  60 },
    };
    int n_sizes = (int)(sizeof sizes / sizeof *sizes);

    for (int i = 0; i < n_sizes; i++) {
        usm_pool_t *p = up_usm_pool_create(8, sizes[i][0], sizes[i][1]);
        CHECK(p != NULL);
        size_t n = (size_t)sizes[i][0] * sizes[i][1];
        uint8_t *src = malloc(n);
        uint8_t *dst = malloc(n);
        CHECK(src && dst);
        if (src && dst) {
            fill_deterministic(src, n, (uint32_t)i);
            up_usm_pool_apply(p, dst, sizes[i][0], src, sizes[i][0], up_usm_amount_pct_to_q8(30));
        }
        free(src); free(dst);
        up_usm_pool_destroy(p);
    }
    END();
}

/* ---- Lifetime test 10: many warm pools alive simultaneously ----
 *
 * Allocates N pools, applies once on each (lazy-init triggers), THEN
 * destroys them in different order. Catches cross-pool aliasing bugs
 * (e.g., a globally-shared resource that gets stolen by the next pool).
 */
static void test_many_pools_alive_simultaneously(void)
{
    BEGIN("8 warm pools alive simultaneously, destroyed in reverse");
    enum { NPOOLS = 8 };
    usm_pool_t *pools[NPOOLS] = {0};
    int width = 64, height = 48;
    size_t n = (size_t)width * height;
    uint8_t *src = malloc(n);
    uint8_t *dst = malloc(n);
    CHECK(src && dst);
    fill_deterministic(src, n, 0xBEEF);

    for (int i = 0; i < NPOOLS; i++) {
        pools[i] = up_usm_pool_create(2 + i, width, height);
        CHECK(pools[i] != NULL);
    }
    /* Apply on each (in order) */
    for (int i = 0; i < NPOOLS; i++) {
        memset(dst, 0xAA, n);
        int rc = up_usm_pool_apply(pools[i], dst, width, src, width,
                                   up_usm_amount_pct_to_q8(30));
        CHECK(rc == 0);
    }
    /* Destroy in REVERSE order to catch any "last pool owns shared
     * state" bug */
    for (int i = NPOOLS - 1; i >= 0; i--) {
        up_usm_pool_destroy(pools[i]);
        pools[i] = NULL;  /* defensive: don't re-touch */
    }
    free(src); free(dst);
    END();
}

int main(void)
{
    printf("Running lifetime / UAF tests...\n");

    test_create_then_destroy_no_apply();
    test_create_apply_destroy();
    test_many_cycles();
    test_alternating_cold_warm();
    test_destroy_null_repeatedly();
    test_create_invalid_no_leak();
    test_input_buffer_can_be_freed();
    test_dst_buffer_swappable();
    test_variable_sizes_in_succession();
    test_many_pools_alive_simultaneously();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    /* If ASan detected leaks they're reported at process exit; the
     * exit code reflects the test pass/fail. */
    return g_fail == 0 ? 0 : 1;
}
