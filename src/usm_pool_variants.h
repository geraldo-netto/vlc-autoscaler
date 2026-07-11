// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm_pool_variants.h - single source of truth for the SIMD-variant
 * entry-point prototypes (ABI-2)
 *****************************************************************************
 * usm_pool.c compiled with -DUSM_VARIANT=<name> renames its public
 * symbols to up_usm_pool_{create,destroy,apply}_<name>. Every consumer
 * (usm_pool_dispatch.c, tests/test_usm_pool_variants.c,
 * tests/fuzz_usm_variants.c) previously hand-declared all nine externs;
 * a prototype change kept in only some copies still compiled per-TU and
 * linked (same symbol name) — silent ABI mismatch at the call boundary.
 *
 * These X-macro-generated declarations are the only copy. usm_pool.c
 * includes this header under -DUSM_VARIANT, so a drift between the
 * declarations here and the real definitions is a compile error in the
 * variant TUs themselves.
 *****************************************************************************/

#ifndef AUTOUPSCALE_USM_POOL_VARIANTS_H
#define AUTOUPSCALE_USM_POOL_VARIANTS_H

#include <stdint.h>

#include "usm_pool.h"

#define UP_USM_POOL_VARIANT_LIST(X) \
    X(sse2)                         \
    X(avx2)                         \
    X(avx512)

#define UP_USM_POOL_DECLARE_VARIANT(name)                                     \
    extern usm_pool_t *up_usm_pool_create_##name(int n_threads, int width,    \
                                                 int height,                  \
                                                 int stripe_min_rows);        \
    extern void up_usm_pool_destroy_##name(usm_pool_t *pool);                 \
    extern int up_usm_pool_apply_##name(usm_pool_t *pool,                     \
                                        uint8_t *dst, int dst_stride,         \
                                        const uint8_t *src, int src_stride,   \
                                        int amount_q8);                       \
    extern int up_usm_pool_effective_threads_##name(const usm_pool_t *pool);

UP_USM_POOL_VARIANT_LIST(UP_USM_POOL_DECLARE_VARIANT)
#undef UP_USM_POOL_DECLARE_VARIANT

#endif /* AUTOUPSCALE_USM_POOL_VARIANTS_H */
