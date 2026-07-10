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
 * We exercise:
 *   - Pool with N=1, 2, 3, 4, 8 workers
 *   - Various dimensions including non-multiples of common values
 *   - Tall narrow, short wide, square
 *   - amount = 0 (identity), 30 (typical), 100 (strong), 256 (max)
 *   - Pseudo-random source data (deterministic seed)
 *****************************************************************************/

#include "../src/usm.h"
#include "../src/usm_pool.h"
#include "barrier_fault_inject.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

/* Compat shim over the io-struct oracle API (Sonar >7-params refactor):
 * preserves this file's original flat-argument call shape. */
static int apply_plane8(uint8_t *dst, int dst_stride,
                        const uint8_t *src, int src_stride,
                        int width, int height,
                        int amount_q8, uint8_t *workspace)
{
    up_usm_plane_io_t io = { dst, dst_stride, src, src_stride, width, height };
    return up_usm_apply_plane(&io, amount_q8, workspace);
}


/*
 * AddressSanitizer aborts the program when an allocation request
 * exceeds its internal hard cap (0x10000000000 = 1 TB). We deliberately
 * trigger a huge `aligned_alloc` in test_apply_lazy_init_oom_sticky to
 * exercise the workspace-alloc failure path; without this override
 * ASan would kill the test runner instead of letting aligned_alloc
 * return NULL. The override only affects this test binary; real
 * production builds have no ASan.
 */
__attribute__((used))
const char *__asan_default_options(void)
{
    return "allocator_may_return_null=1";
}

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_cur_fail = 1; \
        } \
    } while (0)

/* Deterministic xorshift, identical seeding per test for reproducibility. */
static void fill_pseudorandom(uint8_t *buf, size_t n, uint64_t seed)
{
    uint64_t s = seed ? seed : 0xc0ffee123ULL;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        buf[i] = (uint8_t)(s & 0xFF);
    }
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
    apply_plane8(dst_st, width,
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
        apply_plane8(dst_st, width, src, width,
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

static void test_inplace_stride_mismatch_rejected(void)
{
    BEGIN("in-place with mismatched strides is rejected");
    enum { W = 64, H = 64, STRIDE = 80 };
    static uint8_t buf[STRIDE * H];
    usm_pool_t *p = up_usm_pool_create(2, W, H, 0);
    CHECK(p != NULL);
    if (p) {
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
        apply_plane8(dst_st, width, src, width, width, height, amount, ws);
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
    apply_plane8(ref, W, src, W, W, H, amount, ws);

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

/*
 * Coverage for the lazy-init failure paths in usm_pool.c.
 *
 * When `up_usm_pool_apply` first runs (lazy init), it allocates the
 * shared rolling-scratch block (USM_POOL_SCRATCH_ROWS * width bytes per
 * worker) via `aligned_alloc`. Sizing that request past available
 * virtual memory (and past ASan's ~1 TB allocator cap) makes the
 * allocation return NULL — exercising the `lazy_init_failed = true`
 * sticky path. The pool must:
 *   1. return -1 from the first apply()
 *   2. continue to return -1 from subsequent apply() calls without
 *      re-attempting the allocation (the "sticky" contract)
 *   3. destroy cleanly without leaking the partial state
 *
 * Scratch is five rows per worker (the old design allocated a full
 * width*height workspace), so a single worker on an INT_MAX-wide frame is
 * roughly 10 GB and might succeed. We request many workers on a very tall
 * frame so 5 * n_threads * INT_MAX deterministically exceeds
 * the cap; create() keeps all of them because height/stripe_min is huge.
 * The scratch alloc fails before any worker thread is spawned.
 */
static void test_apply_lazy_init_oom_sticky(void)
{
    BEGIN("apply: lazy_init OOM -> sticky failure across subsequent calls");

    /* 3 * 512 * INT_MAX ≈ 3.3 TB — past ASan's ~1 TB cap and past any
     * real machine's memory, so aligned_alloc deterministically fails. */
    usm_pool_t *p = up_usm_pool_create(512, INT_MAX, INT_MAX, 0);
    /* create() doesn't allocate the scratch — only the small priv
     * struct — so it must succeed. */
    CHECK(p != NULL);
    if (!p) { END(); return; }

    /* Tiny dst/src pointers — apply() never reaches them because
     * lazy_init fails before any frame work. Strides match width to
     * pass the pre-init validation. */
    uint8_t scratch[16] = {0};
    int amount = up_usm_amount_pct_to_q8(30);

    int rc1 = up_usm_pool_apply(p, scratch, INT_MAX,
                                 scratch, INT_MAX, amount);
    CHECK(rc1 == -1);

    /* Sticky: a second call must also fail without retry. The pool's
     * `lazy_init_failed` flag is set on the first failed init and
     * short-circuits future calls. */
    int rc2 = up_usm_pool_apply(p, scratch, INT_MAX,
                                 scratch, INT_MAX, amount);
    CHECK(rc2 == -1);

    /* Destroy must clean up safely even though lazy init never
     * completed (no workers spawned, no workspace allocated). */
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
    apply_plane8(expected, W, src, W, W, H, amount, workspace);

    memcpy(inplace, src, bytes);
    CHECK(up_usm_pool_apply(pool, inplace, W, inplace, W, amount) == 0);
    memcpy(inplace, src, bytes);
    barrier_fault_inject_next_dispatch();
    CHECK(up_usm_pool_apply(pool, inplace, W, inplace, W, amount) == -1);
    CHECK(memcmp(inplace, expected, bytes) == 0);
    CHECK(barrier_fault_injection_consumed());

    memset(inplace, 0xA5, bytes);
    CHECK(up_usm_pool_apply(pool, inplace, W, src, W, 0) == -1);
    for (size_t i = 0; i < bytes; i++) CHECK(inplace[i] == 0xA5);

    up_usm_pool_destroy(pool);
    free(src); free(expected); free(inplace); free(workspace);
    END();
}

/*
 * Exercise the pthread_create-failure path in usm_pool_spawn_worker: the
 * spawn loop must stop cleanly and shrink the pool (or fail sticky) with
 * nothing leaked from the failed slot. We
 * force the failure by dropping RLIMIT_NPROC so new threads can't start.
 * Whether the pool comes up with a partial set or none at all, the result
 * must be clean under ASan (no leaked semaphores, no crash). The frame is
 * small and fully backed so a partially-spawned pool can still run safely.
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
    test_apply_lazy_init_oom_sticky();
    test_barrier_failure_drains_and_sticks();
    test_destroy_null_safe();
    test_destroy_unused_pool();
    test_apply_null_pool();
    test_apply_invalid_strides();
    test_spawn_pthread_create_fail_clean();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
