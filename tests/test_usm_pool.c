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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint8_t *dst_st = malloc(plane_bytes);
    uint8_t *dst_mt = malloc(plane_bytes);
    uint8_t *workspace = malloc(plane_bytes);

    if (!src || !dst_st || !dst_mt || !workspace) {
        printf("    malloc failed\n");
        free(src); free(dst_st); free(dst_mt); free(workspace);
        return SIZE_MAX;
    }

    fill_pseudorandom(src, plane_bytes, seed);

    /* Single-threaded reference */
    up_usm_apply_plane(dst_st, width,
                       src, width,
                       width, height,
                       amount_q8,
                       workspace);

    /* Threaded pool */
    usm_pool_t *pool = up_usm_pool_create(n_threads, width, height);
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

/* --------------- core byte-identity tests --------------- */

static void test_identity_amount_zero(void)
{
    BEGIN("amount=0 fast path: pool matches single-threaded (no thread spawn)");
    CHECK(run_compare(4, 200, 100, 0, 0x1) == 0);
    CHECK(run_compare(8, 100, 100, 0, 0x2) == 0);
    END();
}

static void test_typical_30pct(void)
{
    BEGIN("amount=30 (typical default): all worker counts byte-identical");
    int amount = up_usm_amount_pct_to_q8(30);
    CHECK(run_compare(1, 1920, 1080, amount, 0xa) == 0);
    CHECK(run_compare(2, 1920, 1080, amount, 0xb) == 0);
    CHECK(run_compare(3, 1920, 1080, amount, 0xc) == 0);
    CHECK(run_compare(4, 1920, 1080, amount, 0xd) == 0);
    CHECK(run_compare(8, 1920, 1080, amount, 0xe) == 0);
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
    uint8_t *dst_st = malloc(n);
    uint8_t *dst_mt = malloc(n);
    uint8_t *ws = malloc(n);

    usm_pool_t *pool = up_usm_pool_create(4, width, height);
    CHECK(pool != NULL);

    /* Hammer 5 frames, each with different src data. Pool spawns
     * threads on frame 1 and reuses them for frames 2..5. */
    for (int frame = 0; frame < 5; frame++) {
        fill_pseudorandom(src, n, 0x1000 + frame);
        up_usm_apply_plane(dst_st, width, src, width, width, height, amount, ws);
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
    CHECK(up_usm_pool_create( 0, 100, 100) == NULL);
    CHECK(up_usm_pool_create(-1, 100, 100) == NULL);
    CHECK(up_usm_pool_create( 4,   0, 100) == NULL);
    CHECK(up_usm_pool_create( 4, 100,   0) == NULL);
    CHECK(up_usm_pool_create( 4,  -1, 100) == NULL);
    CHECK(up_usm_pool_create( 4, 100,  -1) == NULL);
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
    usm_pool_t *pool = up_usm_pool_create(8, 1920, 1080);
    CHECK(pool != NULL);
    up_usm_pool_destroy(pool);
    /* If this leaked or crashed, the test framework would catch it. */
    END();
}

static void test_apply_null_pool(void)
{
    BEGIN("apply(NULL, ...) returns 0 cleanly");
    uint8_t buf[100];
    CHECK(up_usm_pool_apply(NULL, buf, 10, buf, 10, 64) == 0);
    END();
}

static void test_apply_invalid_strides(void)
{
    BEGIN("apply with stride < width returns 0");
    usm_pool_t *pool = up_usm_pool_create(2, 100, 50);
    CHECK(pool != NULL);
    const uint8_t src[100*50] = {0};
    uint8_t dst[100*50];
    /* dst_stride too small */
    CHECK(up_usm_pool_apply(pool, dst, 50, src, 100, 64) == 0);
    /* src_stride too small */
    CHECK(up_usm_pool_apply(pool, dst, 100, src, 50, 64) == 0);
    up_usm_pool_destroy(pool);
    END();
}

int main(void)
{
    printf("Running usm_pool tests...\n");

    /* Core: byte-identity to single-threaded across many configurations */
    test_identity_amount_zero();
    test_typical_30pct();
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
    test_destroy_null_safe();
    test_destroy_unused_pool();
    test_apply_null_pool();
    test_apply_invalid_strides();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
