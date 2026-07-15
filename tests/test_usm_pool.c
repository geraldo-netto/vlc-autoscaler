// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_usm_pool.c - byte-identity test for threaded USM
 *****************************************************************************
 * The threaded USM pool must produce bit-identical output to
 * up_usm_apply_plane for any (width, height, amount, n_threads, src)
 * combination. This test is the strongest correctness check: if
 * pool output ever differs from single-threaded on the same inputs,
 * the threading has a bug (off-by-one at stripe boundaries, race in
 * workspace access, partition gap, etc.).
 *
 * The cases span worker counts, unusual dimensions and aspect ratios,
 * identity and nonzero amounts, and deterministic pseudo-random input.
 *****************************************************************************/

#include "usm_test_util.h"
#include "../src/usm_pool.h"
#include "../src/threading.h"
#include "barrier_fault_inject.h"
#include "prng.h"   /* DUP-2: shared xorshift32 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static atomic_int g_fail_next_aligned_alloc;
static atomic_uint g_aligned_alloc_calls;
static atomic_size_t g_max_aligned_alloc_size;
static atomic_uint g_pthread_create_calls;

void *__real_aligned_alloc(size_t alignment, size_t size);
void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    atomic_fetch_add_explicit(&g_aligned_alloc_calls, 1,
                              memory_order_relaxed);
    /* Pool allocations happen on the calling thread only, so a plain
     * compare-then-store max is race-free here. */
    if (size > atomic_load_explicit(&g_max_aligned_alloc_size,
                                    memory_order_relaxed))
        atomic_store_explicit(&g_max_aligned_alloc_size, size,
                              memory_order_relaxed);
    if (atomic_exchange_explicit(&g_fail_next_aligned_alloc, 0,
                                 memory_order_relaxed)) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_aligned_alloc(alignment, size);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg)
{
    atomic_fetch_add_explicit(&g_pthread_create_calls, 1,
                              memory_order_relaxed);
    return __real_pthread_create(thread, attr, start, arg);
}

static void reset_pthread_create_calls(void)
{
    atomic_store_explicit(&g_pthread_create_calls, 0, memory_order_relaxed);
}

static unsigned pthread_create_call_count(void)
{
    return atomic_load_explicit(&g_pthread_create_calls,
                                memory_order_relaxed);
}

static void fail_next_aligned_alloc(void)
{
    atomic_store_explicit(&g_fail_next_aligned_alloc, 1,
                          memory_order_relaxed);
}

static unsigned aligned_alloc_call_count(void)
{
    return atomic_load_explicit(&g_aligned_alloc_calls,
                                memory_order_relaxed);
}

static int aligned_alloc_failure_consumed(void)
{
    return atomic_load_explicit(&g_fail_next_aligned_alloc,
                                memory_order_relaxed) == 0;
}

static size_t max_aligned_alloc_size(void)
{
    return atomic_load_explicit(&g_max_aligned_alloc_size,
                                memory_order_relaxed);
}

#include "test_harness.h"

/* Deterministic fill, identical seeding per test for reproducibility. Thin
 * wrapper over the shared PRNG (DUP-2); the 64-bit seed is folded to 32 bits.
 * Values feed pool-vs-single byte-identity checks, so the exact stream is
 * immaterial as long as both sides see the same buffer. */
static void fill_pseudorandom(uint8_t *buf, size_t n, uint64_t seed)
{
    up_fill_random(buf, n, (uint32_t)seed ^ (uint32_t)(seed >> 32));
}

/*
 * Run both single-threaded and pool USM on identical inputs and
 * confirm byte-for-byte match. Returns 0 if equal, count of differing
 * bytes if not.
 */
static size_t run_compare(int n_threads, int width, int height,
                          int amount_q8, uint64_t seed)
{
    size_t plane_bytes = (size_t)width * (size_t)height;
    uint8_t *src   = malloc(plane_bytes);
    /* dst_st/dst_mt zeroed so the diff loop never reads an uninit byte
     * if up_usm_pool_apply leaves bytes unwritten (also satisfies
     * SonarQube's garbage-value check on the `!=` comparison). */
    uint8_t *dst_st = calloc(plane_bytes, 1);
    uint8_t *dst_mt = calloc(plane_bytes, 1);
    uint8_t *workspace = malloc(plane_bytes);

    if (!src || !dst_st || !dst_mt || !workspace) {
        printf("    malloc failed\n");
        free(src); free(dst_st); free(dst_mt); free(workspace);
        return SIZE_MAX;
    }

    fill_pseudorandom(src, plane_bytes, seed);

    /* Single-threaded reference */
    UP_TEST_USM_APPLY_PLANE(dst_st, width,
                       src, width,
                       width, height,
                       amount_q8,
                       workspace);

    /* Threaded pool */
    usm_pool_t *pool = up_usm_pool_create(n_threads, width, height, 0);
    if (!pool) {
        printf("    pool create failed\n");
        free(src); free(dst_st); free(dst_mt); free(workspace);
        return SIZE_MAX;
    }
    up_usm_pool_apply(pool, dst_mt, width, src, width, amount_q8);
    up_usm_pool_destroy(pool);

    /* Compare */
    size_t diff = 0;
    for (size_t i = 0; i < plane_bytes; i++)
        if (dst_st[i] != dst_mt[i]) diff++;

    free(src); free(dst_st); free(dst_mt); free(workspace);
    return diff;
}

/*
 * In-place variant (SYS-4): the pool runs with dst == src (production's
 * call shape in ApplyUsmIfEnabled) and must still match the oracle run
 * on separate buffers. Returns differing-byte count, SIZE_MAX on setup
 * failure.
 */
static size_t run_compare_inplace(int n_threads, int width, int height,
                                  int amount_q8, uint64_t seed)
{
    size_t plane_bytes = (size_t)width * (size_t)height;
    uint8_t *src       = malloc(plane_bytes);
    uint8_t *dst_st    = calloc(plane_bytes, 1);
    uint8_t *inplace   = malloc(plane_bytes);
    uint8_t *workspace = malloc(plane_bytes);
    size_t diff = SIZE_MAX;

    if (src && dst_st && inplace && workspace) {
        fill_pseudorandom(src, plane_bytes, seed);
        UP_TEST_USM_APPLY_PLANE(dst_st, width, src, width,
                           width, height, amount_q8, workspace);

        memcpy(inplace, src, plane_bytes);
        usm_pool_t *pool = up_usm_pool_create(n_threads, width, height, 0);
        if (pool &&
            up_usm_pool_apply(pool, inplace, width, inplace, width,
                              amount_q8) == 0) {
            diff = 0;
            for (size_t i = 0; i < plane_bytes; i++)
                if (dst_st[i] != inplace[i]) diff++;
        }
        up_usm_pool_destroy(pool);
    }

    free(src); free(dst_st); free(inplace); free(workspace);
    return diff;
}

/* --------------- core byte-identity tests --------------- */

static void test_identity_amount_zero(void)
{
    BEGIN("amount=0 fast path: pool matches single-threaded (no thread spawn)");
    CHECK(run_compare(4, 200, 100, 0, 0x1) == 0);
    CHECK(run_compare(8, 100, 100, 0, 0x2) == 0);
    END();
}

/*
 * usm_pool_identity has two branches now: unified-stride fast path
 * (single big memcpy when src_stride == dst_stride == width) and the
 * row-by-row slow path. The fast path is hit in production for all
 * VLC YUV planes whose stride happens to equal the visible width
 * (common at 480p, 720p, 1080p chroma at certain widths). Confirm
 * the slow path still works correctly when called via the pool.
 */
static void test_identity_pool_strided_slow_path(void)
{
    BEGIN("amount=0: pool with stride > width preserves padding");
    enum { W = 13, H = 6, STRIDE = 20 };
    uint8_t src[STRIDE * H], dst[STRIDE * H];

    /* visible payload + sentinel padding */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++)
            src[y * STRIDE + x] = (uint8_t)(y * 41 + x * 19);
        for (int x = W; x < STRIDE; x++)
            src[y * STRIDE + x] = 0xCC;
    }
    memset(dst, 0xAB, sizeof dst);

    usm_pool_t *pool = up_usm_pool_create(4, W, H, 0);
    CHECK(pool != NULL);
    if (!pool) { END(); return; }

    int rc = up_usm_pool_apply(pool, dst, STRIDE, src, STRIDE, 0);
    CHECK(rc == 0);

    /* Visible region: must equal src. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            CHECK(dst[y * STRIDE + x] == src[y * STRIDE + x]);
        }
    }
    /* Dst padding must NOT have been touched (slow path doesn't
     * touch beyond width per row). */
    for (int y = 0; y < H; y++) {
        for (int x = W; x < STRIDE; x++) {
            CHECK(dst[y * STRIDE + x] == 0xAB);
        }
    }

    up_usm_pool_destroy(pool);
    END();
}

static void test_typical_30pct(void)
{
    BEGIN("amount=30 (representative): all worker counts byte-identical");
    int amount = up_usm_amount_pct_to_q8(30);
    CHECK(run_compare(1, 1920, 1080, amount, 0xa) == 0);
    CHECK(run_compare(2, 1920, 1080, amount, 0xb) == 0);
    CHECK(run_compare(3, 1920, 1080, amount, 0xc) == 0);
    CHECK(run_compare(4, 1920, 1080, amount, 0xd) == 0);
    CHECK(run_compare(8, 1920, 1080, amount, 0xe) == 0);
    END();
}

static void test_inplace_matches_oracle(void)
{
    BEGIN("in-place (dst==src): matches separate-buffer oracle at all counts");
    int amount = up_usm_amount_pct_to_q8(30);
    CHECK(run_compare_inplace(1, 1920, 1080, amount, 0x30) == 0);
    CHECK(run_compare_inplace(2, 1920, 1080, amount, 0x31) == 0);
    CHECK(run_compare_inplace(4,  854,  480, amount, 0x32) == 0);
    CHECK(run_compare_inplace(8,  854,  480, amount, 0x33) == 0);
    CHECK(run_compare_inplace(16, 854,  480, amount, 0x34) == 0);
    /* Uneven stripes + max amount: hardest halo case. */
    CHECK(run_compare_inplace(4, 213, 137,
                              up_usm_amount_pct_to_q8(200), 0x35) == 0);
    /* Identity in-place stays a no-op fast path. */
    CHECK(run_compare_inplace(4, 854, 480, 0, 0x36) == 0);
    END();
}

/* A completion-notification failure must fail that dispatch and poison the
 * pool, never hang or silently succeed. */
static void test_completion_signal_failure_poisons_pool(void)
{
    BEGIN("completion-signal failure poisons the pool");
    enum { W = 64, H = 64 };
    static uint8_t buf[W * H];
    memset(buf, 0x40, sizeof buf);
    int amount = up_usm_amount_pct_to_q8(30);
    usm_pool_t *p = up_usm_pool_create(2, W, H, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_apply(p, buf, W, buf, W, amount) == 0);
        barrier_fault_inject_next_done_signal();
        CHECK_EQ(up_usm_pool_apply(p, buf, W, buf, W, amount),
                 UP_USM_APPLY_OUTPUT_UNCERTAIN);
        /* Sticky: the pool stays broken. */
        CHECK_EQ(up_usm_pool_apply(p, buf, W, buf, W, amount),
                 UP_USM_APPLY_FAILED_UNCHANGED);
        up_usm_pool_destroy(p);
    }
    END();
}

/* MEM-2 regression: create() must clamp n_threads at UP_THREADS_MAX even
 * when the stripe-height clamp alone would allow thousands of workers —
 * otherwise a direct caller can drive the worker-array and scratch size
 * multiplies to absurd values (wrapping them on ILP32). Observable via
 * the wrapped aligned_alloc: every pool allocation stays bounded by the
 * clamped worker count, and output stays byte-identical. */
static void test_create_clamps_huge_thread_count(void)
{
    BEGIN("create clamps n_threads at UP_THREADS_MAX (MEM-2)");
    enum { W = 64, H = 100000 };
    atomic_store_explicit(&g_max_aligned_alloc_size, 0,
                          memory_order_relaxed);
    CHECK(run_compare_inplace(INT_MAX, W, H,
                              up_usm_amount_pct_to_q8(30), 0x4d) == 0);
    CHECK(max_aligned_alloc_size() <= (size_t)UP_THREADS_MAX * 4096);
    END();
}

/* PERF-2: the sweep is bandwidth-bound and stops scaling around 10-12 workers
 * (1080p: N=10 is the measured knee at 51 us; N=30 regresses to 76 us), while
 * the stripe floor alone would allow 135 workers at 1080p. The default policy
 * must cap at the plateau — but an explicit stripe_min_rows means the caller
 * has taken over the partition policy and must still get what it asked for. */
static void test_default_policy_caps_useful_threads(void)
{
    BEGIN("default policy caps the pool at the bandwidth knee (PERF-2)");
    enum { USEFUL = 12 };   /* USM_MAX_USEFUL_THREADS */

    /* 1080p: the stripe floor (8 rows) would permit 135 workers. */
    usm_pool_t *p = up_usm_pool_create(64, 1920, 1080, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_effective_threads(p) == USEFUL);
        up_usm_pool_destroy(p);
    }

    /* 4K: same cap, still inside the measured plateau. */
    p = up_usm_pool_create(32, 3840, 2160, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_effective_threads(p) == USEFUL);
        up_usm_pool_destroy(p);
    }

    /* Below the cap, the request is honoured unchanged. */
    p = up_usm_pool_create(4, 1920, 1080, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_effective_threads(p) == 4);
        up_usm_pool_destroy(p);
    }

    /* Explicit stripe_min_rows opts out of the cap: 1080/8 = 135 -> 64. */
    p = up_usm_pool_create(64, 1920, 1080, 8);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_effective_threads(p) == 64);
        up_usm_pool_destroy(p);
    }

    /* The height clamp still wins when it is the tighter bound. */
    p = up_usm_pool_create(64, 100, 16, 0);
    CHECK(p != NULL);
    if (p) {
        CHECK(up_usm_pool_effective_threads(p) == 2);   /* 16/8 */
        up_usm_pool_destroy(p);
    }

    /* Capping must not change a single byte of output. */
    CHECK(run_compare_inplace(64, 640, 360, up_usm_amount_pct_to_q8(30),
                              0x61) == 0);
    END();
}

/* PERF-1: a single-stripe pool must not spawn a thread at all — the whole
 * point is to skip the broadcast + sem round-trip (~7 us/dispatch, measured)
 * that buys nothing with one worker. The pthread_create wrapper proves it
 * directly; the byte-identity checks below prove the inline sweep produces
 * exactly what the threaded pool would have. */

static void test_single_thread_runs_inline(void)
{
    BEGIN("n_threads=1 sweeps on the calling thread, spawning none (PERF-1)");
    enum { W = 256, H = 128 };
    const int amount = up_usm_amount_pct_to_q8(30);
    static uint8_t buf[W * H];

    reset_pthread_create_calls();
    usm_pool_t *p = up_usm_pool_create(1, W, H, 0);
    CHECK(p != NULL);
    CHECK_EQ(pthread_create_call_count(), 0);
    if (!p) { END(); return; }

    /* The pool is lazy: the first apply is what would have spawned. Run
     * several frames — the inline slot must be reusable. */
    for (int frame = 0; frame < 3; frame++)
        CHECK(up_usm_pool_apply(p, buf, W, buf, W, amount) == 0);

    CHECK(up_usm_pool_effective_threads(p) == 1);
    CHECK_EQ(pthread_create_call_count(), 0);

    up_usm_pool_destroy(p);
    CHECK_EQ(pthread_create_call_count(), 0);

    /* And the inline sweep is byte-identical to the threaded pool's output
     * (both already match the single-threaded oracle). */
    CHECK(run_compare(1, 640, 360, amount, 0x51) == 0);
    CHECK(run_compare_inplace(1, 640, 360, amount, 0x52) == 0);
    CHECK(run_compare(1, 63, 17, amount, 0x53) == 0);   /* odd dims */
    CHECK_EQ(pthread_create_call_count(), 0);
    END();
}

static void test_inplace_stride_mismatch_rejected(void)
{
    BEGIN("in-place with mismatched strides is rejected");
    enum { W = 64, H = 64, STRIDE = 80 };
    usm_pool_t *p = up_usm_pool_create(2, W, H, 0);
    CHECK(p != NULL);
    if (p) {
        static uint8_t buf[STRIDE * H];
        CHECK(up_usm_pool_apply(p, buf, STRIDE, buf, W,
                                up_usm_amount_pct_to_q8(30)) == -1);
        up_usm_pool_destroy(p);
    }
    END();
}

static void test_aggressive_100pct(void)
{
    BEGIN("amount=100% (max-clamped to 256/256): all worker counts match");
    int amount = up_usm_amount_pct_to_q8(100);
    CHECK(run_compare(1, 1280, 720, amount, 0x10) == 0);
    CHECK(run_compare(4, 1280, 720, amount, 0x11) == 0);
    CHECK(run_compare(8, 1280, 720, amount, 0x12) == 0);
    END();
}

/* --------------- boundary / edge cases --------------- */

static void test_odd_dimensions(void)
{
    BEGIN("odd dimensions: 213x137 with 4 threads (uneven stripe partition)");
    /* Height 137, N=4 -> stripes of 34, 34, 34, 35 rows. Tests that the
     * last stripe absorbing the remainder works correctly. */
    int amount = up_usm_amount_pct_to_q8(30);
    CHECK(run_compare(4, 213, 137, amount, 0x20) == 0);
    END();
}

static void test_tall_narrow(void)
{
    BEGIN("tall narrow: 32x2000 with 8 threads");
    int amount = up_usm_amount_pct_to_q8(50);
    CHECK(run_compare(8, 32, 2000, amount, 0x30) == 0);
    END();
}

static void test_short_wide(void)
{
    BEGIN("short wide: 4096x32 with 4 threads (32/4=8 rows/stripe = min)");
    int amount = up_usm_amount_pct_to_q8(50);
    CHECK(run_compare(4, 4096, 32, amount, 0x40) == 0);
    END();
}

static void test_height_clamps_thread_count(void)
{
    BEGIN("requesting 64 threads on h=16: pool clamps to h/8 = 2 internally, "
          "still byte-identical");
    int amount = up_usm_amount_pct_to_q8(30);
    /* Height 16, USM_STRIPE_MIN_ROWS=8 -> max 2 stripes. Pool reduces
     * silently and partition still covers all rows. */
    CHECK(run_compare(64, 100, 16, amount, 0x50) == 0);
    END();
}

static void test_n_threads_equal_height(void)
{
    BEGIN("n_threads close to height/8 (smallest legal stripes)");
    int amount = up_usm_amount_pct_to_q8(30);
    /* Height 64, N=8 -> exactly 8 rows per stripe. Boundary-heaviest case. */
    CHECK(run_compare(8, 100, 64, amount, 0x60) == 0);
    END();
}

static void test_repeat_same_pool(void)
{
    BEGIN("same pool reused across multiple frames: lazy_init cached");
    int width = 320, height = 240;
    int amount = up_usm_amount_pct_to_q8(40);

    size_t n = (size_t)width * height;
    uint8_t *src = malloc(n);
    /* See run_compare(): zero-init silences SonarQube's garbage-value
     * read on dst_mt[i] in the inner diff loop. */
    uint8_t *dst_st = calloc(n, 1);
    uint8_t *dst_mt = calloc(n, 1);
    uint8_t *ws = malloc(n);

    if (!src || !dst_st || !dst_mt || !ws) {
        printf("    malloc failed\n");
        free(src); free(dst_st); free(dst_mt); free(ws);
        g_cur_fail = 1;
        END();
        return;
    }

    usm_pool_t *pool = up_usm_pool_create(4, width, height, 0);
    CHECK(pool != NULL);

    /* Hammer 5 frames, each with different src data. Pool spawns
     * threads on frame 1 and reuses them for frames 2..5. */
    for (int frame = 0; frame < 5; frame++) {
        fill_pseudorandom(src, n, 0x1000 + frame);
        UP_TEST_USM_APPLY_PLANE(dst_st, width, src, width, width, height, amount, ws);
        up_usm_pool_apply(pool, dst_mt, width, src, width, amount);

        size_t diff = 0;
        for (size_t i = 0; i < n; i++) if (dst_st[i] != dst_mt[i]) diff++;
        if (diff != 0) {
            printf("    frame %d: %zu bytes differ\n", frame, diff);
            g_cur_fail = 1;
        }
    }

    up_usm_pool_destroy(pool);
    free(src); free(dst_st); free(dst_mt); free(ws);
    END();
}

/* --------------- pool API safety --------------- */

static void test_create_invalid_args(void)
{
    BEGIN("create with invalid args returns NULL");
    CHECK(up_usm_pool_create( 0, 100, 100, 0) == NULL);
    CHECK(up_usm_pool_create(-1, 100, 100, 0) == NULL);
    CHECK(up_usm_pool_create( 4,   0, 100, 0) == NULL);
    CHECK(up_usm_pool_create( 4, 100,   0, 0) == NULL);
    CHECK(up_usm_pool_create( 4,  -1, 100, 0) == NULL);
    CHECK(up_usm_pool_create( 4, 100,  -1, 0) == NULL);
    END();
}

static void test_destroy_null_safe(void)
{
    BEGIN("destroy(NULL) is safe");
    up_usm_pool_destroy(NULL);  /* must not crash */
    END();
}

static void test_destroy_unused_pool(void)
{
    BEGIN("destroy a pool that never had apply() called: no thread leak");
    /* The pool spawns threads lazily on apply(). If we destroy without
     * calling apply, no threads were spawned and destroy must still be
     * clean (no pthread_join on uninitialized state). */
    usm_pool_t *pool = up_usm_pool_create(8, 1920, 1080, 0);
    CHECK(pool != NULL);
    up_usm_pool_destroy(pool);
    /* If this leaked or crashed, the test framework would catch it. */
    END();
}

/* OBS-1: the query is what callers log instead of the thread count they
 * asked for, so it must expose the create-time clamp and stay stable
 * across a fully-spawned apply. */
static void test_effective_threads_query(void)
{
    BEGIN("effective_threads: NULL=0, create clamp visible, stable after apply");
    CHECK(up_usm_pool_effective_threads(NULL) == 0);

    /* Height 16, stripe_min 8 -> clamped to 2 despite asking for 64. */
    usm_pool_t *clamped = up_usm_pool_create(64, 100, 16, 0);
    CHECK(clamped != NULL);
    if (clamped) {
        CHECK(up_usm_pool_effective_threads(clamped) == 2);
        up_usm_pool_destroy(clamped);
    }

    enum { W = 64, H = 64 };
    usm_pool_t *p = up_usm_pool_create(4, W, H, 0);
    CHECK(p != NULL);
    if (p) {
        static uint8_t buf[W * H];
        CHECK(up_usm_pool_effective_threads(p) == 4);
        CHECK(up_usm_pool_apply(p, buf, W, buf, W,
                                up_usm_amount_pct_to_q8(30)) == 0);
        CHECK(up_usm_pool_effective_threads(p) == 4);
        up_usm_pool_destroy(p);
    }
    END();
}

static void test_apply_null_pool(void)
{
    BEGIN("apply(NULL, ...) returns -1 cleanly");
    /* Zero-init silences -Wmaybe-uninitialized in non-ASan builds. */
    uint8_t buf[100] = {0};
    CHECK(up_usm_pool_apply(NULL, buf, 10, buf, 10, 64) == -1);
    END();
}

static void test_apply_invalid_strides(void)
{
    BEGIN("apply with stride < width returns -1");
    usm_pool_t *pool = up_usm_pool_create(2, 100, 50, 0);
    CHECK(pool != NULL);
    const uint8_t src[100*50] = {0};
    uint8_t dst[100*50];
    /* dst_stride too small */
    CHECK(up_usm_pool_apply(pool, dst, 50, src, 100, 64) == -1);
    /* src_stride too small */
    CHECK(up_usm_pool_apply(pool, dst, 100, src, 50, 64) == -1);
    up_usm_pool_destroy(pool);
    END();
}

/*
 * Boundary coverage for the stripe_min_rows constructor argument
 * (`--autoupscale-usm-stripe-min-rows`, VLC range 0..256).
 *
 * Contract:
 *   stripe_min_rows <=  0   -> compile-time default (USM_STRIPE_MIN_ROWS=8)
 *   stripe_min_rows  >= 1   -> used as-is to clamp n_threads to height/value
 *
 * Test: for each boundary value, create the pool and verify apply()
 * produces byte-identical output to the single-threaded reference.
 * Out-of-VLC-range values (negative, > 256) are exercised defensively
 * — VLC clamps before us, but the API must not crash if a direct
 * caller passes them.
 */
static void test_create_stripe_min_rows_boundaries(void)
{
    BEGIN("create stripe_min_rows: -1/0/1/8/256 boundaries + INT_MIN/INT_MAX");
    enum { W = 64, H = 64, N = 4 };
    int amount = up_usm_amount_pct_to_q8(30);

    uint8_t *src = malloc((size_t)W * H);
    uint8_t *dst = malloc((size_t)W * H);
    /* Zero-init ref so the diff loop never reads garbage if the
     * single-threaded apply_plane skips a byte (SonarQube cross-TU). */
    uint8_t *ref = calloc((size_t)W * H, 1);
    uint8_t *ws  = malloc((size_t)W * H);
    if (!src || !dst || !ref || !ws) {
        printf("    malloc failed\n"); g_cur_fail++;
        free(src); free(dst); free(ref); free(ws); END(); return;
    }
    fill_pseudorandom(src, (size_t)W * H, 0xB0BABEEFu);
    UP_TEST_USM_APPLY_PLANE(ref, W, src, W, W, H, amount, ws);

    int boundaries[] = {
        INT_MIN, -1, 0,            /* sentinels: all map to default 8 */
        1, 4, 8,                   /* near and at compile-time default */
        16, 32, 256,               /* mid range and VLC max */
        257,                       /* one above VLC max */
        1000000, INT_MAX           /* pathologically large */
    };

    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        usm_pool_t *p = up_usm_pool_create(N, W, H, boundaries[i]);
        CHECK(p != NULL);
        if (!p) continue;
        memset(dst, 0xAB, (size_t)W * H);
        CHECK(up_usm_pool_apply(p, dst, W, src, W, amount) == 0);
        /* Output must match the single-threaded reference regardless
         * of how stripe_min_rows shrinks the worker count. */
        for (int j = 0; j < W * H; j++) {
            if (dst[j] != ref[j]) {
                printf("    stripe_min_rows=%d: byte %d differs "
                       "(dst=0x%02x ref=0x%02x)\n",
                       boundaries[i], j, dst[j], ref[j]);
                g_cur_fail++;
                break;
            }
        }
        up_usm_pool_destroy(p);
    }
    free(src); free(dst); free(ref); free(ws);
    END();
}

static void test_apply_lazy_init_alloc_failure_sticky(void)
{
    BEGIN("apply: injected lazy-init allocation failure is sticky");
    enum { W = 8, H = 8 };
    const uint8_t src[W * H] = {0};
    uint8_t dst[W * H];
    uint8_t unchanged[W * H];
    memset(dst, 0xA5, sizeof dst);
    memcpy(unchanged, dst, sizeof dst);
    usm_pool_t *p = up_usm_pool_create(1, W, H, 0);
    CHECK(p != NULL);
    if (!p) { END(); return; }

    int amount = up_usm_amount_pct_to_q8(30);
    unsigned calls_before = aligned_alloc_call_count();
    fail_next_aligned_alloc();

    int rc1 = up_usm_pool_apply(p, dst, W, src, W, amount);
    CHECK(rc1 == UP_USM_APPLY_FAILED_UNCHANGED);
    CHECK(memcmp(dst, unchanged, sizeof dst) == 0);
    CHECK(aligned_alloc_failure_consumed());
    CHECK(aligned_alloc_call_count() == calls_before + 1);

    unsigned calls_after_failure = aligned_alloc_call_count();
    int rc2 = up_usm_pool_apply(p, dst, W, src, W, amount);
    CHECK(rc2 == UP_USM_APPLY_FAILED_UNCHANGED);
    CHECK(memcmp(dst, unchanged, sizeof dst) == 0);
    CHECK(aligned_alloc_call_count() == calls_after_failure);

    up_usm_pool_destroy(p);
    END();
}

static void test_barrier_failure_drains_and_sticks(void)
{
    BEGIN("barrier failure drains frame, stops pool, and sticks");
    enum { W = 64, H = 64 };
    size_t bytes = (size_t)W * H;
    uint8_t *src = malloc(bytes);
    uint8_t *expected = malloc(bytes);
    uint8_t *inplace = malloc(bytes);
    uint8_t *workspace = malloc(bytes);
    usm_pool_t *pool = up_usm_pool_create(4, W, H, 0);
    if (!src || !expected || !inplace || !workspace || !pool) {
        printf("    setup failed\n");
        g_cur_fail = 1;
        free(src); free(expected); free(inplace); free(workspace);
        up_usm_pool_destroy(pool);
        END();
        return;
    }

    int amount = up_usm_amount_pct_to_q8(30);
    fill_pseudorandom(src, bytes, 0xC04B4AULL);
    UP_TEST_USM_APPLY_PLANE(expected, W, src, W, W, H, amount, workspace);

    memcpy(inplace, src, bytes);
    CHECK(up_usm_pool_apply(pool, inplace, W, inplace, W, amount) == 0);
    memcpy(inplace, src, bytes);
    barrier_fault_inject_next_dispatch();
    CHECK(up_usm_pool_apply(pool, inplace, W, inplace, W, amount)
          == UP_USM_APPLY_OUTPUT_UNCERTAIN);
    CHECK(memcmp(inplace, expected, bytes) == 0);
    CHECK(barrier_fault_injection_consumed());

    memset(inplace, 0xA5, bytes);
    CHECK(up_usm_pool_apply(pool, inplace, W, src, W, 0)
          == UP_USM_APPLY_FAILED_UNCHANGED);
    for (size_t i = 0; i < bytes; i++) CHECK(inplace[i] == 0xA5);

    up_usm_pool_destroy(pool);
    free(src); free(expected); free(inplace); free(workspace);
    END();
}

static void test_worker_gate_failure_marks_output_uncertain(void)
{
    BEGIN("worker gate failure marks partially written output uncertain");
    enum { W = 64, H = 64 };
    const uint8_t src[W * H] = {0};
    uint8_t dst[W * H];
    uint8_t unchanged[W * H];
    memset(dst, 0xA5, sizeof dst);
    memcpy(unchanged, dst, sizeof dst);

    usm_pool_t *pool = up_usm_pool_create(4, W, H, 0);
    CHECK(pool != NULL);
    if (!pool) { END(); return; }

    barrier_fault_inject_next_worker_mutex_lock();
    CHECK(up_usm_pool_apply(pool, dst, W, src, W,
                            up_usm_amount_pct_to_q8(30))
          == UP_USM_APPLY_OUTPUT_UNCERTAIN);
    CHECK(barrier_fault_injection_consumed());

    size_t written = 0;
    size_t untouched = 0;
    size_t unexpected = 0;
    for (size_t i = 0; i < sizeof dst; i++) {
        if (dst[i] == 0) written++;
        else if (dst[i] == 0xA5) untouched++;
        else unexpected++;
    }
    CHECK(written > 0);
    CHECK(untouched > 0);
    CHECK(unexpected == 0);

    memset(dst, 0xA5, sizeof dst);
    CHECK(up_usm_pool_apply(pool, dst, W, src, W,
                            up_usm_amount_pct_to_q8(30))
          == UP_USM_APPLY_FAILED_UNCHANGED);
    CHECK(memcmp(dst, unchanged, sizeof dst) == 0);

    up_usm_pool_destroy(pool);
    END();
}

/*
 * Exercise the pthread_create-failure path in usm_pool_spawn_worker: the
 * spawn loop must stop cleanly and shrink the pool (or fail sticky) with
 * nothing leaked from the failed slot. We
 * force the failure by dropping RLIMIT_NPROC so new threads can't start.
 * Whether the pool comes up with a partial set or none at all, the result
 * must be clean under ASan (no leaked synchronization resources, no crash).
 * The frame is small and fully backed so a partially-spawned pool can still
 * run safely.
 */
static void test_spawn_pthread_create_fail_clean(void)
{
    BEGIN("lazy_init survives pthread_create failure and frees the worker slot");
    enum { W = 64, H = 512 };
    struct rlimit old;
    if (getrlimit(RLIMIT_NPROC, &old) != 0) { END(); return; }
    struct rlimit lim = old;
    lim.rlim_cur = 1;   /* below the live thread count -> pthread_create EAGAIN */
    if (setrlimit(RLIMIT_NPROC, &lim) != 0) { END(); return; }

    uint8_t *src = calloc((size_t)W * H, 1);
    uint8_t *dst = calloc((size_t)W * H, 1);
    usm_pool_t *p = up_usm_pool_create(64, W, H, 0);
    int rc = (src && dst && p)
        ? up_usm_pool_apply(p, dst, W, src, W, up_usm_amount_pct_to_q8(30))
        : -1;

    setrlimit(RLIMIT_NPROC, &old);   /* restore before join/destroy */

    /* -1 (no worker spawned, sticky) or 0 (partial pool ran) are both
     * acceptable; the point is no leak/crash, which ASan enforces. */
    CHECK(rc == -1 || rc == 0);
    /* OBS-1: after a partial spawn ran, the query reports the workers
     * that actually exist, never more than requested. */
    if (rc == 0) {
        int eff = up_usm_pool_effective_threads(p);
        CHECK(eff >= 1 && eff <= 64);
    }
    up_usm_pool_destroy(p);
    free(src); free(dst);
    END();
}

int main(void)
{
    printf("Running usm_pool tests...\n");

    /* Core: byte-identity to single-threaded across many configurations */
    test_identity_amount_zero();
    test_identity_pool_strided_slow_path();
    test_typical_30pct();
    test_inplace_matches_oracle();
    test_completion_signal_failure_poisons_pool();
    test_create_clamps_huge_thread_count();
    test_default_policy_caps_useful_threads();
    test_single_thread_runs_inline();
    test_inplace_stride_mismatch_rejected();
    test_aggressive_100pct();

    /* Boundary cases */
    test_odd_dimensions();
    test_tall_narrow();
    test_short_wide();
    test_height_clamps_thread_count();
    test_n_threads_equal_height();
    test_repeat_same_pool();

    /* API safety */
    test_create_invalid_args();
    test_create_stripe_min_rows_boundaries();
    test_apply_lazy_init_alloc_failure_sticky();
    test_barrier_failure_drains_and_sticks();
    test_worker_gate_failure_marks_output_uncertain();
    test_destroy_null_safe();
    test_destroy_unused_pool();
    test_effective_threads_query();
    test_apply_null_pool();
    test_apply_invalid_strides();
    test_spawn_pthread_create_fail_clean();

    return test_harness_report();
}
