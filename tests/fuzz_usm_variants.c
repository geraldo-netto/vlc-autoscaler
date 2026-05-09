// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * fuzz_usm_variants.c - cross-SIMD-variant byte-equivalence fuzzer
 *****************************************************************************
 * For each fuzz input, run all three USM pool variants (SSE2/AVX2/AVX-512)
 * on the same source data and assert byte-identical output. Catches any
 * SIMD-width-specific bug that the deterministic test_usm_pool_variants
 * tests might miss with their hand-picked sizes.
 *
 * Two build modes (same pattern as fuzz_usm.c):
 *
 *   - libFuzzer target (default with clang -fsanitize=fuzzer)
 *   - Smoke runner (-DFUZZ_MAIN), 100k iterations by default.
 *
 * On non-AVX-512 / non-AVX2 CPUs, the corresponding variants are skipped.
 *****************************************************************************/

#include "../src/usm_pool.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern usm_pool_t *up_usm_pool_create_sse2(int, int, int, int);
extern void        up_usm_pool_destroy_sse2(usm_pool_t *);
extern int         up_usm_pool_apply_sse2(usm_pool_t *, uint8_t *, int,
                                          const uint8_t *, int, int);
extern usm_pool_t *up_usm_pool_create_avx2(int, int, int, int);
extern void        up_usm_pool_destroy_avx2(usm_pool_t *);
extern int         up_usm_pool_apply_avx2(usm_pool_t *, uint8_t *, int,
                                          const uint8_t *, int, int);
extern usm_pool_t *up_usm_pool_create_avx512(int, int, int, int);
extern void        up_usm_pool_destroy_avx512(usm_pool_t *);
extern int         up_usm_pool_apply_avx512(usm_pool_t *, uint8_t *, int,
                                            const uint8_t *, int, int);

/* Bound dimensions to keep memory and time reasonable per iteration.
 * The test suite (test_usm_pool_variants) covers larger sizes deterministically;
 * fuzzing focuses on small inputs with high variation. */
#define FUZZ_MAX_W 192
#define FUZZ_MAX_H 192

/* CPU feature flags, populated lazily on first call. */
static int features_init = 0;
static int has_avx2 = 0;
static int has_avx512 = 0;

static void init_features(void)
{
    if (features_init) return;
    __builtin_cpu_init();
    has_avx2   = __builtin_cpu_supports("avx2");
    has_avx512 = __builtin_cpu_supports("avx512f")
              && __builtin_cpu_supports("avx512bw");
    features_init = 1;
}

/* ABORT semantics: byte-equivalence is a HARD invariant. If two variants
 * disagree, that's a real bug and we want it to surface immediately under
 * libFuzzer (which detects abort()) and under ASan in the smoke build. */
static void check_equal_or_abort(const uint8_t *a, const uint8_t *b, size_t n,
                                 const char *label, int w, int h, int amount, int workers)
{
    if (memcmp(a, b, n) == 0) return;
    fprintf(stderr,
            "%s differs from SSE2: w=%d h=%d amount=%d workers=%d\n",
            label, w, h, amount, workers);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            fprintf(stderr,
                    "  first diff at offset %zu: sse2=0x%02x %s=0x%02x\n",
                    i, a[i], label, b[i]);
            break;
        }
    }
    abort();
}

static void run_one(const uint8_t *data, size_t size)
{
    if (size < 8) return;
    init_features();

    uint16_t u_w, u_h;
    int16_t  i_amount;
    uint8_t  u_workers;
    uint8_t  u_seed;

    memcpy(&u_w,       data + 0, 2);
    memcpy(&u_h,       data + 2, 2);
    memcpy(&i_amount,  data + 4, 2);
    u_workers = data[6];
    u_seed    = data[7];

    int width   = (u_w % FUZZ_MAX_W) + 1;
    int height  = (u_h % FUZZ_MAX_H) + 1;
    int amount  = ((int)i_amount % 513) - 1;   /* -1..511 — pool clamps to [0, 256] */
    if (amount < 0) amount = 0;
    int workers = (u_workers % 6) + 1;          /* 1..6 workers */

    size_t n = (size_t)width * (size_t)height;
    uint8_t *src        = malloc(n);
    uint8_t *dst_sse2   = malloc(n);
    uint8_t *dst_avx2   = malloc(n);
    uint8_t *dst_avx512 = malloc(n);
    if (!src || !dst_sse2 || !dst_avx2 || !dst_avx512) {
        free(src); free(dst_sse2); free(dst_avx2); free(dst_avx512);
        return;
    }

    /* Seed source pattern from u_seed and any tail of the fuzz input. */
    uint32_t s = 0x9E3779B9u ^ (uint32_t)u_seed;
    if (size > 8) {
        for (size_t k = 8; k < size && k < 8 + 16; k++) s = (s << 5) ^ data[k];
    }
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        src[i] = (uint8_t)(s >> 16);
    }

    /* Establish reference output via SSE2 (always available). */
    memset(dst_sse2, 0xCC, n);
    {
        usm_pool_t *p = up_usm_pool_create_sse2(workers, width, height, 0);
        if (!p) goto out;
        up_usm_pool_apply_sse2(p, dst_sse2, width, src, width, amount);
        up_usm_pool_destroy_sse2(p);
    }

    if (has_avx2) {
        memset(dst_avx2, 0xCC, n);
        usm_pool_t *p = up_usm_pool_create_avx2(workers, width, height, 0);
        if (p) {
            up_usm_pool_apply_avx2(p, dst_avx2, width, src, width, amount);
            up_usm_pool_destroy_avx2(p);
            check_equal_or_abort(dst_sse2, dst_avx2, n, "avx2",
                                 width, height, amount, workers);
        }
    }

    if (has_avx512) {
        memset(dst_avx512, 0xCC, n);
        usm_pool_t *p = up_usm_pool_create_avx512(workers, width, height, 0);
        if (p) {
            up_usm_pool_apply_avx512(p, dst_avx512, width, src, width, amount);
            up_usm_pool_destroy_avx512(p);
            check_equal_or_abort(dst_sse2, dst_avx512, n, "avx512",
                                 width, height, amount, workers);
        }
    }

out:
    free(src); free(dst_sse2); free(dst_avx2); free(dst_avx512);
}

/* ---------- libFuzzer entry point ---------- */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    run_one(data, size);
    return 0;
}

/* ---------- standalone smoke main ---------- */
#ifdef FUZZ_MAIN
int main(int argc, char **argv)
{
    /* 5000 default: each iteration creates 3 worker pools (one per variant)
     * with up to 6 workers each, and runs under ASan in the smoke build.
     * That's ~90k thread spawn/joins per 5k iters. Higher counts are
     * practical via explicit argument or in the libFuzzer build. */
    long n = 5000;
    if (argc > 1) {
        char *end = NULL;
        long v = strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v > 0) n = v;
    }

    init_features();
    printf("Variant fuzz: AVX2=%s AVX512=%s\n",
           has_avx2 ? "yes" : "no",
           has_avx512 ? "yes" : "no");

    uint32_t s = 0xBADD00Du;
    uint8_t buf[32];
    for (long i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof buf; j += 4) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            memcpy(buf + j, &s, 4);
        }
        run_one(buf, sizeof buf);
    }
    printf("USM variant cross-equivalence smoke fuzz OK: %ld iterations\n", n);
    return 0;
}
#endif
