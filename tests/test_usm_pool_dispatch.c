// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * test_usm_pool_dispatch.c - unit tests for the runtime SIMD dispatcher
 *****************************************************************************
 * Exercises src/usm_pool_dispatch.c directly (x86-64 only; the TU itself has
 * an #error guard for other architectures). Stub variant entry points give the
 * dispatch table real addresses to bind, so the test can assert which variant
 * is selected for each capability combination and that the public shims
 * forward through it — without pulling in the real SIMD kernels.
 *****************************************************************************/

#include <stdint.h>
#include <string.h>

#include "../src/usm_pool.h"
#include "../src/usm_pool_variants.h"

struct usm_pool_s {
    uint8_t sentinel;
};

static usm_pool_t stub_pool;

/* Define the nine suffixed entry points the dispatcher binds to. They are
 * inert: the tests check selection and forwarding, never the kernels. */
#define DEFINE_STUB_VARIANT(name)                                             \
    usm_pool_t *up_usm_pool_create_##name(int n_threads, int width,           \
                                          int height, int stripe_min_rows)    \
    { (void)n_threads; (void)width; (void)height; (void)stripe_min_rows;      \
      return &stub_pool; }                                                    \
    void up_usm_pool_destroy_##name(usm_pool_t *pool) { (void)pool; }         \
    int up_usm_pool_apply_##name(usm_pool_t *pool, uint8_t *dst,              \
                                 int dst_stride, const uint8_t *src,          \
                                 int src_stride, int amount_q8)               \
    { (void)pool; (void)dst; (void)dst_stride; (void)src; (void)src_stride;   \
      (void)amount_q8; return 0; }                                            \
    int up_usm_pool_effective_threads_##name(const usm_pool_t *pool)          \
    { (void)pool; return 7; }
UP_USM_POOL_VARIANT_LIST(DEFINE_STUB_VARIANT)

#include "../src/usm_pool_dispatch.c"

#include "test_harness.h"

static void test_select_ops_prefers_highest_isa(void)
{
    BEGIN("select_ops picks avx512 > avx2 > sse2 by capability");
    usm_pool_ops_t ops;
    const char *name = NULL;

    usm_pool_select_ops(1, 1, &ops, &name);
    CHECK(strcmp(name, "avx512") == 0);
    CHECK(ops.create == up_usm_pool_create_avx512);

    usm_pool_select_ops(0, 1, &ops, &name);
    CHECK(strcmp(name, "avx2") == 0);
    CHECK(ops.create == up_usm_pool_create_avx2);

    usm_pool_select_ops(0, 0, &ops, &name);
    CHECK(strcmp(name, "sse2") == 0);
    CHECK(ops.create == up_usm_pool_create_sse2);
    END();
}

static void test_public_api_forwards(void)
{
    BEGIN("public API forwards through the load-selected variant");
    /* The constructor already ran at load and replaced the initial
     * "uninitialized" sentinel with the host's variant name. */
    CHECK(strcmp(up_usm_pool_variant_name, "uninitialized") != 0);

    usm_pool_t *pool = up_usm_pool_create(4, 64, 64, 8);
    CHECK(pool != NULL);
    uint8_t dst[16] = { 0 };
    const uint8_t src[16] = { 0 };
    CHECK(up_usm_pool_apply(pool, dst, 16, src, 16, 128) == 0);
    CHECK(up_usm_pool_effective_threads(pool) == 7);
    up_usm_pool_destroy(pool);
    END();
}

static up_cpu_x86_features_t full_v4_features(void)
{
    return (up_cpu_x86_features_t) {
        .max_basic = 7,
        .max_ext = 0x80000001u,
        .leaf1_ecx = UP_X86_1_ECX_SSE3 | UP_X86_1_ECX_SSSE3
            | UP_X86_1_ECX_FMA | UP_X86_1_ECX_CX16
            | UP_X86_1_ECX_SSE41 | UP_X86_1_ECX_SSE42
            | UP_X86_1_ECX_MOVBE | UP_X86_1_ECX_POPCNT
            | UP_X86_1_ECX_OSXSAVE | UP_X86_1_ECX_AVX
            | UP_X86_1_ECX_F16C,
        .leaf1_edx = UP_X86_1_EDX_SSE2,
        .leaf7_ebx = UP_X86_7_EBX_BMI | UP_X86_7_EBX_AVX2
            | UP_X86_7_EBX_BMI2 | UP_X86_7_EBX_AVX512F
            | UP_X86_7_EBX_AVX512DQ | UP_X86_7_EBX_AVX512CD
            | UP_X86_7_EBX_AVX512BW | UP_X86_7_EBX_AVX512VL,
        .ext1_ecx = UP_X86_EXT1_ECX_LAHF | UP_X86_EXT1_ECX_LZCNT,
        .xcr0 = UINT64_C(0xe6),
    };
}

static void test_cpu_level_fallback_requires_complete_v3(void)
{
    BEGIN("cpu-level fallback requires complete inherited v2/v3 features");
    up_cpu_x86_features_t f = full_v4_features();
    CHECK(up_cpu_features_support_v3(&f));

    f = full_v4_features();
    f.leaf1_ecx &= ~UP_X86_1_ECX_F16C;
    CHECK(!up_cpu_features_support_v3(&f));

    f = full_v4_features();
    f.leaf1_ecx &= ~UP_X86_1_ECX_MOVBE;
    CHECK(!up_cpu_features_support_v3(&f));

    f = full_v4_features();
    f.ext1_ecx &= ~UP_X86_EXT1_ECX_LZCNT;
    CHECK(!up_cpu_features_support_v3(&f));

    f = full_v4_features();
    f.ext1_ecx &= ~UP_X86_EXT1_ECX_LAHF;
    CHECK(!up_cpu_features_support_v3(&f));
    END();
}

static void test_cpu_level_fallback_requires_os_state(void)
{
    BEGIN("cpu-level fallback requires AVX/AVX-512 OS save state");
    up_cpu_x86_features_t f = full_v4_features();
    f.xcr0 = UINT64_C(0x2);
    CHECK(!up_cpu_features_support_v3(&f));

    f = full_v4_features();
    f.xcr0 = UINT64_C(0x6);
    CHECK(up_cpu_features_support_v3(&f));
    CHECK(!up_cpu_features_support_v4(&f));

    f = full_v4_features();
    CHECK(up_cpu_features_support_v4(&f));
    END();
}

int main(void)
{
    test_select_ops_prefers_highest_isa();
    test_public_api_forwards();
    test_cpu_level_fallback_requires_complete_v3();
    test_cpu_level_fallback_requires_os_state();
    return test_harness_report();
}
