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
 * create() allocates only the small priv struct; private scratch is allocated
 * and worker threads are spawned lazily on the first apply() call.
 *
 * At the public API level, amount_q8 == 0 takes a fast identity path with no
 * thread or scratch setup. The plugin omits the pool entirely when USM is off.
 *****************************************************************************/

/*
 * !!! IF YOU ADD OR REMOVE A PUBLIC FUNCTION HERE !!!
 *
 * src/usm_pool_dispatch.c hand-forwards every public symbol to one of
 * three SIMD-baseline-compiled variants (sse2/avx2/avx512). It does NOT
 * use IFUNC/target_clones, so adding a new public function here without
 * also adding a forwarding shim in usm_pool_dispatch.c and an entry in
 * usm_pool_variants.h's declaration macro will produce an unresolved
 * symbol at .so load.
 *
 * Conversely, deleting a function here without deleting it from the
 * dispatcher will produce three orphaned symbols and a linker error.
 *
 * Prototype changes are covered: all variant-suffix declarations live
 * only in usm_pool_variants.h, and usm_pool.c includes it under
 * -DUSM_VARIANT, so declaration/definition drift is a compile error.
 */

#ifndef AUTOUPSCALE_USM_POOL_H
#define AUTOUPSCALE_USM_POOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct usm_pool_s usm_pool_t;

#define UP_USM_APPLY_OK                 0
#define UP_USM_APPLY_FAILED_UNCHANGED  -1
#define UP_USM_APPLY_OUTPUT_UNCERTAIN  -2

/*
 * Create a USM pool sized for width * height frames with up to
 * n_threads workers, capped at 64 and max(1, height/stripe_min_rows) to
 * keep each stripe at least stripe_min_rows rows tall when possible.
 * Pass stripe_min_rows <= 0 to use the compile-time default (8).
 * CPU limits and automatic worker policy are the caller's responsibility.
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
 * (SYS-4). Same-base/different-stride calls are rejected; other partial
 * overlap (different base pointers whose ranges overlap) is unsupported.
 * Returns UP_USM_APPLY_OK on success,
 * UP_USM_APPLY_FAILED_UNCHANGED when validation or lazy initialization fails
 * before dst is touched, and UP_USM_APPLY_OUTPUT_UNCERTAIN when dispatch fails
 * after workers may have written dst. The latter drains and joins the workers,
 * leaves the pool in a sticky failed state, and requires the caller to discard
 * the destination frame. The completion wait is timed, but safe retirement is
 * synchronous and may wait for an already-running worker callback; this API
 * does not promise a hard end-to-end execution deadline.
 *
 * (Convention matches the rest of the project: 0 = success, negative
 * = failure. Was inverted in earlier versions; flipped 2026-05.)
 *
 * Output is bit-identical to up_usm_apply_plane(dst, dst_stride, src,
 * src_stride, width, height, amount_q8, workspace) for any inputs.
 *
 * Normal user-derived amount_q8 values are 0..UP_USM_AMOUNT_Q8_NORMAL_MAX.
 * The API defensively clamps values outside [0, UP_USM_AMOUNT_Q8_MAX]
 * (matching the single-threaded behavior). amount_q8 == 0 is a fast
 * identity copy with no thread or workspace activity.
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
 * Effective worker count (OBS-1): the create-time clamped count until
 * the first apply(); afterwards it also reflects any partial-spawn
 * shrink from lazy init. Returns 0 for a NULL pool. Callers logging
 * pool parallelism should query this after the first apply(), not echo
 * the worker count they requested.
 */
int up_usm_pool_effective_threads(const usm_pool_t *pool);

/*
 * Name of the active SIMD variant, for diagnostic logging. Exactly one
 * strong definition is linked, depending on build mode (ABI-1):
 *   - MULTIVERSION=1: usm_pool_dispatch.c sets it at .so load to the chosen
 *     variant ("avx512" / "avx2" / "sse2") through the shared full-level CPU
 *     probes.
 *   - MULTIVERSION=0: usm_pool.c (compiled without USM_VARIANT) defines it as
 *     "default" — the single baseline reflects the build-time -march level,
 *     which the user already chose, so no runtime detection is done.
 * The two definitions are mutually exclusive (guarded by USM_VARIANT), so
 * there is no clash and no weak alias is involved.
 */
extern const char *up_usm_pool_variant_name;

#endif /* AUTOUPSCALE_USM_POOL_H */
