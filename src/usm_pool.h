/*****************************************************************************
 * usm_pool.h - threaded USM (unsharp mask) post-pass
 *****************************************************************************
 * Wraps a persistent pool of worker threads that apply USM to a luma
 * plane in horizontal stripes. Bit-identical output to up_usm_apply_plane
 * for any combination of (width, height, amount_q8, src) inputs.
 *
 * Algorithm: two-phase dispatch with a barrier between phases.
 *   Phase 1: each worker hblurs its assigned rows into a shared workspace.
 *   Barrier: main thread sem_waits all N pass-1 done signals.
 *   Phase 2: each worker combines src + workspace[y-1, y, y+1] -> dst on
 *            its assigned rows. Workspace reads are race-free because
 *            phase 1 fully completed before any phase 2 work began.
 *
 * The pool follows the same lifecycle as the zimg backend: cheap
 * create() allocates only the small priv struct; the workspace and
 * worker threads are spawned lazily on the first apply() call.
 *
 * For amount_q8 == 0 (USM disabled) apply() takes a fast identity path
 * with NO thread spawn or workspace allocation - the pool is essentially
 * free at runtime when USM is off.
 *****************************************************************************/

#ifndef AUTOUPSCALE_USM_POOL_H
#define AUTOUPSCALE_USM_POOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct usm_pool_s usm_pool_t;

/*
 * Create a USM pool sized for width * height frames with up to
 * n_threads workers. n_threads is clamped to [1, height/8] to keep
 * each stripe at least 8 rows tall (the kernel boundary handling
 * makes thinner stripes wasteful). Returns NULL on invalid args
 * (n_threads <= 0, width <= 0, height <= 0) or allocation failure.
 *
 * Does NOT spawn worker threads or allocate the workspace. Those
 * happen on the first up_usm_pool_apply() call.
 */
usm_pool_t *up_usm_pool_create(int n_threads, int width, int height);

/*
 * Apply USM to one frame. dst and src may alias if dst_stride ==
 * src_stride (the identity/amount=0 case). Returns 1 on success, 0
 * if the pool is NULL, the strides are too small, or lazy thread
 * spawn fails.
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
 * Safe to call on a pool that never had apply() invoked.
 */
void up_usm_pool_destroy(usm_pool_t *pool);

#endif /* AUTOUPSCALE_USM_POOL_H */
