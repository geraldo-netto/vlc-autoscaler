// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_usm_pool_variants.c - cross-SIMD-variant byte-equivalence test
 *****************************************************************************
 * The plugin ships three SIMD variants of the USM pool (SSE2 / AVX2 /
 * AVX-512), selected at runtime by usm_pool_dispatch.c. They MUST produce
 * byte-identical output for every input — the kernels do bytewise
 * saturating arithmetic, so wider SIMD just runs more lanes in parallel,
 * not different math.
 *
 * This test exercises that invariant directly: same source through all
 * three variants, byte-compare. It catches:
 *
 *   - Lane-handling bugs in vectorized clamp / saturation
 *   - Partition / boundary issues that only show up at one SIMD width
 *   - Aliasing or restrict violations exposed by aggressive optimization
 *   - SIMD-specific arithmetic differences (none should exist for our
 *     bytewise clamped ops, but we verify rather than assume)
 *
 * Each variant is conditionally exercised using the dispatcher's shared
 * level probes:
 *   - SSE2 always (every x86_64 has it)
 *   - AVX2 when the CPU satisfies x86-64-v3
 *   - AVX-512 when the CPU satisfies x86-64-v4
 *
 * The test is a NO-OP on non-AVX-512 CPUs for the AVX-512 variant, etc.
 *****************************************************************************/

#include "../src/cpu_level.h"
#include "../src/usm.h"
#include "../src/usm_pool.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Variant entry points: one shared set of prototypes (ABI-2). The
 * definitions live in the per-variant .o files (built with -DUSM_VARIANT). */
#include "../src/usm_pool_variants.h"

static int g_run = 0, g_fail = 0, g_cur_fail = 0;
static const char *g_cur = NULL;

#define BEGIN(name) do { g_cur = name; g_cur_fail = 0; g_run++; } while (0)
#define END() do { \
        if (g_cur_fail) { g_fail++; printf("  [FAIL] %s\n", g_cur); } \
        else            { printf("  [ ok ] %s\n", g_cur); } \
    } while (0)
#define CHECK(cond) do { \
        if (!(cond)) { g_cur_fail++; \
            printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* CPU feature flags, populated once in main(). */
static int has_avx2 = 0;
static int has_avx512 = 0;

/* Generate a deterministic but varied pixel pattern. */
static void fill_pattern(uint8_t *buf, size_t n, uint32_t seed)
{
    /* Linear congruential — deterministic across runs and platforms. */
    uint32_t s = seed | 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 16);
    }
}

/* Run the SSE2 variant (always available on any x86_64). */
static int run_sse2(uint8_t *dst, const uint8_t *src,
                    int n_workers, int w, int h, int amount_q8)
{
    usm_pool_t *p = up_usm_pool_create_sse2(n_workers, w, h, 0);
    if (!p) return -1;
    int rc = up_usm_pool_apply_sse2(p, dst, w, src, w, amount_q8);
    up_usm_pool_destroy_sse2(p);
    return rc;
}

static int run_avx2(uint8_t *dst, const uint8_t *src,
                    int n_workers, int w, int h, int amount_q8)
{
    usm_pool_t *p = up_usm_pool_create_avx2(n_workers, w, h, 0);
    if (!p) return -1;
    int rc = up_usm_pool_apply_avx2(p, dst, w, src, w, amount_q8);
    up_usm_pool_destroy_avx2(p);
    return rc;
}

static int run_avx512(uint8_t *dst, const uint8_t *src,
                      int n_workers, int w, int h, int amount_q8)
{
    usm_pool_t *p = up_usm_pool_create_avx512(n_workers, w, h, 0);
    if (!p) return -1;
    int rc = up_usm_pool_apply_avx512(p, dst, w, src, w, amount_q8);
    up_usm_pool_destroy_avx512(p);
    return rc;
}

/* Print the offset and bytes of the first divergence between two buffers.
 * `label` names the variant under test (e.g. "avx2", "avx512"). */
static void report_first_diff(const char *label,
                              const uint8_t *baseline,
                              const uint8_t *candidate,
                              size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (baseline[i] == candidate[i]) continue;
        printf("    first diff at offset %zu: sse2=0x%02x %s=0x%02x\n",
               i, baseline[i], label, candidate[i]);
        return;
    }
}

/* Run one SIMD variant and verify byte-identical to the SSE2 baseline.
 * `runner` is the variant's apply entry point. */
typedef int (*variant_runner_t)(uint8_t *, const uint8_t *,
                                int, int, int, int);

static void compare_variant(const char *label,
                            variant_runner_t runner,
                            const uint8_t *src,
                            uint8_t *dst, size_t n,
                            int n_workers, int w, int h, int amount_q8,
                            const uint8_t *baseline)
{
    memset(dst, 0xCC, n);
    CHECK(runner(dst, src, n_workers, w, h, amount_q8) >= 0);
    if (memcmp(baseline, dst, n) == 0) return;
    /* variant diverges from SSE2 baseline */
    CHECK(0);
    report_first_diff(label, baseline, dst, n);
}

/* Compare all three variants for one (size, amount, workers, seed) tuple. */
static void check_one(int n_workers, int w, int h, int amount_q8, uint32_t seed)
{
    size_t n = (size_t)w * (size_t)h;
    uint8_t *src = malloc(n);
    uint8_t *dst_sse2 = malloc(n);
    uint8_t *dst_avx2 = malloc(n);
    uint8_t *dst_avx512 = malloc(n);

    if (!src || !dst_sse2 || !dst_avx2 || !dst_avx512) {
        /* malloc failed */ CHECK(0);
        free(src); free(dst_sse2); free(dst_avx2); free(dst_avx512);
        return;
    }

    fill_pattern(src, n, seed);

    /* SSE2 baseline — always run. Failure here means the test is broken. */
    memset(dst_sse2, 0xCC, n);
    CHECK(run_sse2(dst_sse2, src, n_workers, w, h, amount_q8) >= 0);

    if (has_avx2)
        compare_variant("avx2", run_avx2, src, dst_avx2, n,
                        n_workers, w, h, amount_q8, dst_sse2);
    if (has_avx512)
        compare_variant("avx512", run_avx512, src, dst_avx512, n,
                        n_workers, w, h, amount_q8, dst_sse2);

    free(src); free(dst_sse2); free(dst_avx2); free(dst_avx512);
}

/* ===========================================================================
 * Test cases
 * ===========================================================================*/

static void test_amount_zero(void)
{
    /* amount=0 takes the identity fast path — verify it's identical too. */
    BEGIN("amount=0 (identity fast path) all variants");
    check_one(2, 320, 240, 0, 0xDEADBEEF);
    check_one(2, 1920, 1080, 0, 0xCAFEBABE);
    END();
}

static void test_typical_resolutions(void)
{
    /* Real-world output sizes. */
    BEGIN("480p, amount=51 (default 20%), 2 workers");
    check_one(2, 854, 480, 51, 0x12345678);
    END();

    BEGIN("720p, amount=51, 2 workers");
    check_one(2, 1280, 720, 51, 0x23456789);
    END();

    BEGIN("1080p, amount=51, 2 workers");
    check_one(2, 1920, 1080, 51, 0x34567890);
    END();

    BEGIN("1440p, amount=51, 2 workers");
    check_one(2, 2560, 1440, 51, 0x45678901);
    END();

    BEGIN("4K, amount=51, 2 workers");
    check_one(2, 3840, 2160, 51, 0x56789012);
    END();
}

static void test_amount_sweep(void)
{
    /* Cover the normal user ceiling and defensive API clamp boundaries. */
    BEGIN("amount sweep across normal and defensive API bounds");
    const int amounts[] = {
        -1, 0, 16, 76, 128, 256,
        UP_USM_AMOUNT_Q8_NORMAL_MAX,
        UP_USM_AMOUNT_Q8_MAX,
        UP_USM_AMOUNT_Q8_MAX + 1,
    };
    for (size_t i = 0; i < sizeof(amounts)/sizeof(*amounts); i++) {
        check_one(2, 1920, 1080, amounts[i], 0x11111111u + (uint32_t)i);
    }
    END();
}

static void test_worker_counts(void)
{
    /* Verify the partitioning is consistent across worker counts. The pool
     * may down-clamp worker count internally if the height can't be split
     * cleanly; what matters is that all variants make the same decision. */
    BEGIN("varying worker counts at 1080p");
    int workers[] = { 1, 2, 4, 8 };
    for (size_t i = 0; i < sizeof(workers)/sizeof(*workers); i++) {
        check_one(workers[i], 1920, 1080, 76, 0x77777777u + (uint32_t)i);
    }
    END();
}

static void test_unusual_dimensions(void)
{
    /* Non-multiples-of-SIMD-width and non-multiples-of-stripe-count.
     * SIMD-width=64 (AVX-512) can have boundary issues if we don't
     * handle width % 64 != 0 correctly. */
    BEGIN("widths NOT multiples of 64 (AVX-512 boundary handling)");
    /* widths chosen to leave 1, 7, 31, 63 trailing pixels under 64-byte SIMD */
    int widths[] = { 65, 71, 95, 127, 129, 191, 255, 257 };
    for (size_t i = 0; i < sizeof(widths)/sizeof(*widths); i++) {
        check_one(1, widths[i], 64, 76, 0xAAAAAAAAu + (uint32_t)i);
    }
    END();

    BEGIN("very narrow / very tall");
    check_one(1, 17,  500, 76, 0xBBBBBBBB);  /* narrower than any SIMD vec */
    check_one(1, 1280, 17, 76, 0xCCCCCCCC);  /* fewer rows than workers */
    check_one(1,  1,    1, 76, 0xDDDDDDDD);  /* degenerate edge */
    END();

    BEGIN("very small frames");
    check_one(2, 32,  32, 76, 0xE0E0E0E0);
    check_one(2, 64,  64, 76, 0xE1E1E1E1);
    check_one(4, 128, 128, 76, 0xE2E2E2E2);
    END();
}

/* Run one prepared source buffer through all available variants and
 * byte-compare. Centralizes the alloc/run/cmp/free dance so each pattern
 * case stays linear. */
static void check_pattern(const uint8_t *src, int w, int h,
                          int n_workers, int amount_q8)
{
    size_t n = (size_t)w*h;
    uint8_t *dst1 = malloc(n), *dst2 = malloc(n), *dst3 = malloc(n);
    run_sse2(dst1, src, n_workers, w, h, amount_q8);
    if (has_avx2) {
        run_avx2(dst2, src, n_workers, w, h, amount_q8);
        CHECK(memcmp(dst1, dst2, n) == 0);
    }
    if (has_avx512) {
        run_avx512(dst3, src, n_workers, w, h, amount_q8);
        CHECK(memcmp(dst1, dst3, n) == 0);
    }
    free(dst1); free(dst2); free(dst3);
}

static void test_content_patterns(void)
{
    /* Different content might exercise different code paths in the
     * vectorizer (e.g., constant content vs. high-frequency noise). */
    BEGIN("constant content (mid grey)");
    /* All-grey: blur is identical, so sharpening should be 0 → pass-through. */
    int w = 256, h = 256;
    size_t n = (size_t)w*h;
    uint8_t *src = malloc(n);
    memset(src, 128, n);
    check_pattern(src, w, h, 2, 76);
    free(src);
    END();

    BEGIN("checkerboard (high-frequency)");
    w = 256; h = 256; n = (size_t)w*h;
    src = malloc(n);
    for (size_t i = 0; i < n; i++) src[i] = ((i + i/(size_t)w) & 1) ? 255 : 0;
    check_pattern(src, w, h, 2, 128);
    free(src);
    END();

    BEGIN("ramp (gradient — exercises clamp at edges)");
    w = 256; h = 8; n = (size_t)w*h;
    src = malloc(n);
    for (size_t i = 0; i < n; i++) src[i] = (uint8_t)(i & 0xff);
    check_pattern(src, w, h, 1, 200);
    free(src);
    END();
}

static void test_dispatcher_init(void)
{
    /* The dispatcher exposes up_usm_pool_variant_name. Verify it's been
     * populated with one of the expected values after .so load. */
    BEGIN("dispatcher variant_name is populated");
    CHECK(up_usm_pool_variant_name != NULL);
    if (up_usm_pool_variant_name) {
        int valid = (strcmp(up_usm_pool_variant_name, "sse2")    == 0) ||
                    (strcmp(up_usm_pool_variant_name, "avx2")    == 0) ||
                    (strcmp(up_usm_pool_variant_name, "avx512")  == 0) ||
                    (strcmp(up_usm_pool_variant_name, "default") == 0);
        CHECK(valid);
        printf("    variant_name = \"%s\"\n", up_usm_pool_variant_name);
    }
    END();
}

int main(void)
{
    __builtin_cpu_init();
    /* Level probes, not headline features: the variant kernels are compiled
     * at -march=x86-64-v3/v4, so running them needs the full level (PORT-6). */
    has_avx2   = up_cpu_supports_v3();
    has_avx512 = up_cpu_supports_v4();

    printf("CPU feature gating:\n");
    printf("  AVX2:     %s\n", has_avx2 ? "available — testing variant" : "absent — variant skipped");
    printf("  AVX-512:  %s\n", has_avx512 ? "available — testing variant" : "absent — variant skipped");
    printf("\n");

    test_dispatcher_init();
    test_amount_zero();
    test_typical_resolutions();
    test_amount_sweep();
    test_worker_counts();
    test_unusual_dimensions();
    test_content_patterns();

    printf("\n%d tests run, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
