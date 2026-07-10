// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm_pool.h - threaded USM (unsharp mask) post-pass
 *****************************************************************************
 * Wraps a persistent pool of worker threads that apply USM to a luma
 * plane in horizontal stripes. Bit-identical output to up_usm_apply_plane
 * for any combination of (width, height, amount_q8, src) inputs.
 *
 * Algorithm: fused single-pass sweep — one dispatch per frame; each
 * worker hblurs into three private rolling row buffers and combines on
 * the fly, re-hblurring stripe-boundary rows locally so no worker reads
 * another's memory (full rationale in usm_pool.c's header note).
 *
 * The pool follows the same lifecycle as the zimg backend: cheap
 * create() allocates only the small priv struct; the workspace and
 * worker threads are spawned lazily on the first apply() call.
 *
 * For amount_q8 == 0 (USM disabled) apply() takes a fast identity path
 * with NO thread spawn or workspace allocation - the pool is essentially
 * free at runtime when USM is off.
 *****************************************************************************/

/*
 * !!! IF YOU ADD OR REMOVE A PUBLIC FUNCTION HERE !!!
 *
 * src/usm_pool_dispatch.c hand-forwards every public symbol to one of
 * three SIMD-baseline-compiled variants (sse2/avx2/avx512). It does NOT
 * use IFUNC/target_clones, so adding a new public function here without
 * also adding a forwarding shim and three extern decls in
 * usm_pool_dispatch.c will produce an unresolved symbol at .so load.
 *
 * Conversely, deleting a function here without deleting it from the
 * dispatcher will produce three orphaned symbols and a linker error.
 *
 * The SAME applies when changing the prototype: keep the three extern
 * decls in usm_pool_dispatch.c in sync.
 *
 * tests/test_usm_pool_variants.c and tests/fuzz_usm_variants.c also
 * declare extern prototypes for the variant suffixes; update those too.
 */

#ifndef AUTOUPSCALE_USM_POOL_H
#define AUTOUPSCALE_USM_POOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct usm_pool_s usm_pool_t;

/*
 * Create a USM pool sized for width * height frames with up to
 * n_threads workers. n_threads is clamped to [1, height/stripe_min_rows]
 * to keep each stripe at least stripe_min_rows rows tall (the kernel
 * boundary handling makes thinner stripes wasteful). Pass
 * stripe_min_rows <= 0 to use the compile-time default (8).
 *
 * Returns NULL on invalid args (n_threads <= 0, width <= 0, height <= 0)
 * or allocation failure.
 *
 * Does NOT spawn worker threads or allocate the workspace. Those
 * happen on the first up_usm_pool_apply() call.
 */
usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows);

/*
 * Apply USM to one frame. IN-PLACE IS SUPPORTED for any amount: dst may
 * equal src exactly (same base pointer, same stride) — the pool
 * snapshots each stripe's two boundary halo rows before dispatch so
 * neighbouring workers never read a row another worker is writing
 * (SYS-4). Partial overlap (dst != src but ranges overlapping, or same
 * base with different strides) is rejected/undefined. Returns 0 on
 * success, -1 if the pool is NULL, the strides are too small or
 * mismatched while aliased, or lazy thread spawn fails.
 *
 * (Convention matches the rest of the project: 0 = success, negative
 * = failure. Was inverted in earlier versions; flipped 2026-05.)
 *
 * Output is bit-identical to up_usm_apply_plane(dst, dst_stride, src,
 * src_stride, width, height, amount_q8, workspace) for any inputs.
 *
 * amount_q8 outside [0, UP_USM_AMOUNT_Q8_MAX] is clamped (matches the
 * single-threaded behavior). amount_q8 == 0 is a fast identity copy
 * with no thread or workspace activity.
 */
int up_usm_pool_apply(usm_pool_t *pool,
                      uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      int amount_q8);

/*
 * Free the pool. Joins worker threads if they were ever spawned.
 * Passing NULL is a no-op. Safe to call on a pool that never had
 * apply() invoked.
 */
void up_usm_pool_destroy(usm_pool_t *pool);

/*
 * Name of the active SIMD variant, for diagnostic logging. Exactly one
 * strong definition is linked, depending on build mode (ABI-1):
 *   - MULTIVERSION=1: usm_pool_dispatch.c sets it at .so load to the chosen
 *     variant ("avx512" / "avx2" / "sse2") via __builtin_cpu_supports().
 *   - MULTIVERSION=0: usm_pool.c (compiled without USM_VARIANT) defines it as
 *     "default" — the single baseline reflects the build-time -march level,
 *     which the user already chose, so no runtime detection is done.
 * The two definitions are mutually exclusive (guarded by USM_VARIANT), so
 * there is no clash and no weak alias is involved.
 */
extern const char *up_usm_pool_variant_name;

#endif /* AUTOUPSCALE_USM_POOL_H */
