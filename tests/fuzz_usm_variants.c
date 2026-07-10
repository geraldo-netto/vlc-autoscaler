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
 *   - Smoke runner (-DFUZZ_MAIN), 5k iterations by default.
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

/* Decoded fuzz parameters for a single iteration. */
typedef struct {
    int width;
    int height;
    int amount;
    int workers;
    uint8_t seed;
} fuzz_params_t;

static void decode_params(const uint8_t *data, fuzz_params_t *p)
{
    uint16_t u_w, u_h;
    int16_t  i_amount;

    memcpy(&u_w,      data + 0, 2);
    memcpy(&u_h,      data + 2, 2);
    memcpy(&i_amount, data + 4, 2);

    p->width   = (u_w % FUZZ_MAX_W) + 1;
    p->height  = (u_h % FUZZ_MAX_H) + 1;
    /* Exercise the API's signed input domain around both normal and
     * defensive ranges; every variant must apply the same clamp. */
    p->amount  = (int)i_amount;
    p->workers = (data[6] % 6) + 1;            /* 1..6 workers */
    p->seed    = data[7];
}

static void fill_source(uint8_t *src, size_t n,
                        const uint8_t *data, size_t size, uint8_t seed)
{
    uint32_t s = 0x9E3779B9u ^ (uint32_t)seed;
    if (size > 8) {
        for (size_t k = 8; k < size && k < 8 + 16; k++) s = (s << 5) ^ data[k];
    }
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        src[i] = (uint8_t)(s >> 16);
    }
}

/* Run SSE2 reference. Returns 0 on pool-create failure, 1 on success. */
static int run_sse2_reference(uint8_t *dst, const uint8_t *src, size_t n,
                              const fuzz_params_t *p)
{
    memset(dst, 0xCC, n);
    usm_pool_t *pool = up_usm_pool_create_sse2(p->workers, p->width, p->height, 0);
    if (!pool) return 0;
    up_usm_pool_apply_sse2(pool, dst, p->width, src, p->width, p->amount);
    up_usm_pool_destroy_sse2(pool);
    return 1;
}

static void run_avx2_and_check(uint8_t *dst, const uint8_t *dst_ref,
                               const uint8_t *src, size_t n,
                               const fuzz_params_t *p)
{
    memset(dst, 0xCC, n);
    usm_pool_t *pool = up_usm_pool_create_avx2(p->workers, p->width, p->height, 0);
    if (!pool) return;
    up_usm_pool_apply_avx2(pool, dst, p->width, src, p->width, p->amount);
    up_usm_pool_destroy_avx2(pool);
    check_equal_or_abort(dst_ref, dst, n, "avx2",
                         p->width, p->height, p->amount, p->workers);
}

static void run_avx512_and_check(uint8_t *dst, const uint8_t *dst_ref,
                                 const uint8_t *src, size_t n,
                                 const fuzz_params_t *p)
{
    memset(dst, 0xCC, n);
    usm_pool_t *pool = up_usm_pool_create_avx512(p->workers, p->width, p->height, 0);
    if (!pool) return;
    up_usm_pool_apply_avx512(pool, dst, p->width, src, p->width, p->amount);
    up_usm_pool_destroy_avx512(pool);
    check_equal_or_abort(dst_ref, dst, n, "avx512",
                         p->width, p->height, p->amount, p->workers);
}

static void run_one(const uint8_t *data, size_t size)
{
    if (size < 8) return;
    init_features();

    fuzz_params_t p;
    decode_params(data, &p);

    size_t n = (size_t)p.width * (size_t)p.height;
    uint8_t *src        = malloc(n);
    uint8_t *dst_sse2   = malloc(n);
    uint8_t *dst_avx2   = malloc(n);
    uint8_t *dst_avx512 = malloc(n);
    if (!src || !dst_sse2 || !dst_avx2 || !dst_avx512) goto out;

    fill_source(src, n, data, size, p.seed);

    if (!run_sse2_reference(dst_sse2, src, n, &p)) goto out;
    if (has_avx2)   run_avx2_and_check(dst_avx2, dst_sse2, src, n, &p);
    if (has_avx512) run_avx512_and_check(dst_avx512, dst_sse2, src, n, &p);

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
