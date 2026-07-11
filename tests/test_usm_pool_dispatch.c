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

/* Define the nine suffixed entry points the dispatcher binds to. They are
 * inert: the tests check selection and forwarding, never the kernels. */
#define DEFINE_STUB_VARIANT(name)                                             \
    usm_pool_t *up_usm_pool_create_##name(int n_threads, int width,           \
                                          int height, int stripe_min_rows)    \
    { (void)n_threads; (void)width; (void)height; (void)stripe_min_rows;      \
      return (usm_pool_t *)(uintptr_t)0x1; }                                  \
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
    /* The constructor already ran at load and named the host's variant. */
    CHECK(up_usm_pool_variant_name != NULL);
    CHECK(strcmp(up_usm_pool_variant_name, "uninitialized") != 0);

    usm_pool_t *pool = up_usm_pool_create(4, 64, 64, 8);
    CHECK(pool != NULL);
    uint8_t dst[16] = { 0 };
    uint8_t src[16] = { 0 };
    CHECK(up_usm_pool_apply(pool, dst, 16, src, 16, 128) == 0);
    CHECK(up_usm_pool_effective_threads(pool) == 7);
    up_usm_pool_destroy(pool);
    END();
}

int main(void)
{
    test_select_ops_prefers_highest_isa();
    test_public_api_forwards();
    return test_harness_report();
}
