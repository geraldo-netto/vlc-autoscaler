// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * usm_pool.c - persistent worker pool for threaded USM post-pass
 *****************************************************************************
 * See usm_pool.h for the API and algorithm description.
 *
 * Implementation notes:
 *
 * - Each worker is bound to a contiguous y-row range [y_start, y_end).
 *   Stripes partition [0, height) with no gaps and no overlap. With N
 *   workers, stripe i is [i*h/N, (i+1)*h/N) using integer division;
 *   the last stripe absorbs any rounding remainder.
 *
 * - FUSED SINGLE-PASS sweep. Earlier versions ran two dispatch phases
 *   (all workers hblur into a shared width*height workspace; a barrier;
 *   then all workers combine). That cost two sem round-trips per frame
 *   and two full passes over the luma plane. We now fuse both into one
 *   dispatch: each worker keeps THREE private rolling hblur row buffers
 *   (the rows y-1, y, y+1 it currently needs) and combines on the fly.
 *   Boundary rows shared with a neighbour stripe are simply re-hblurred
 *   locally — hblur is a deterministic per-row function, so the output
 *   stays byte-identical to the single-threaded up_usm_apply_plane. The
 *   private buffers also mean workers never read each other's memory, so
 *   no inter-thread barrier is needed within a frame.
 *
 * - One pool allocation provides five width-sized rows per worker: three
 *   rolling blur rows plus two in-place halo snapshots. Lazy init can also
 *   fail while allocating worker slots or initializing the shared gate/barrier.
 *   If no thread can spawn the failure is sticky; a partial spawn succeeds
 *   with a smaller, repartitioned pool.
 *
 * - The descriptor is cheap; worker storage, synchronization, and threads are
 *   initialized on the first non-identity apply so probe-only Open/Close
 *   cycles cost almost nothing.
 *
 * - amount_q8 == 0 short-circuits to an identity copy with no thread
 *   activity. This matches up_usm_apply_plane's behavior and keeps the
 *   pool cheap when USM is configured off.
 *****************************************************************************/

#include "usm_pool.h"
#include "usm.h"
#include "threading.h"
#include "worker_pool.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Multi-versioning support: when this TU is compiled with -DUSM_VARIANT=name,
 * the public functions get suffixed (e.g. up_usm_pool_create -> up_usm_pool_create_avx2).
 * The plugin links three copies of this file (sse2 / avx2 / avx512), each at
 * its own -march, plus usm_pool_dispatch.c which selects one at .so load time.
 *
 * Without USM_VARIANT defined, the original names are used — that's the build
 * mode for the unit tests and for MULTIVERSION=0 single-baseline plugin builds.
 */
#ifdef USM_VARIANT
#  define USM_PASTE_(a, b) a##_##b
#  define USM_PASTE(a, b)  USM_PASTE_(a, b)
#  define up_usm_pool_create   USM_PASTE(up_usm_pool_create,   USM_VARIANT)
#  define up_usm_pool_destroy  USM_PASTE(up_usm_pool_destroy,  USM_VARIANT)
#  define up_usm_pool_apply    USM_PASTE(up_usm_pool_apply,    USM_VARIANT)
#  define up_usm_pool_effective_threads \
          USM_PASTE(up_usm_pool_effective_threads, USM_VARIANT)
#  define usm_pool_run_worker \
          USM_PASTE(usm_pool_run_worker, USM_VARIANT)
#  if defined(__has_attribute)
#    if __has_attribute(noclone)
#      define USM_ISA_ANCHOR __attribute__((used, noinline, noclone))
#    else
#      define USM_ISA_ANCHOR __attribute__((used, noinline))
#    endif
#  else
#    define USM_ISA_ANCHOR __attribute__((used, noinline))
#  endif
   /* ABI-2: cross-check this TU's renamed definitions against the shared
    * variant prototypes — a drift is a compile error here, not silent ABI
    * mismatch at the call boundary. (The pasted _<name> tokens below are
    * distinct from the object-like macros above, so no re-expansion.) */
#  include "usm_pool_variants.h"
#else
#  define USM_ISA_ANCHOR
   /* Single-baseline build: provide the variant_name symbol that callers
    * (e.g. autoupscale.c's engagement log) expect. The dispatcher provides
    * a strong definition in MULTIVERSION=1 plugin builds; here we provide
    * it as a normal definition for tests and single-baseline plugin builds.
    *
    * The string is "default" because we have no information about which
    * SIMD level was actually compiled — that's a build-time choice (MARCH)
    * the user already knows. Unit tests don't need a real value. */
   const char *up_usm_pool_variant_name = "default";
#endif

/* Each stripe at least this many rows tall; smaller stripes are
 * dominated by kernel boundary handling and not worth threading. */
#define USM_STRIPE_MIN_ROWS 8

/* PERF-2: the pass is memory-bandwidth-bound; measured throughput knees at
 * ~10 workers at 1080p and plateaus at 8-12 at 4K, then regresses. See the
 * rationale in up_usm_pool_create. Exposed for the test that pins it. */
#define USM_MAX_USEFUL_THREADS 12

/* Workspace alignment - matches the rest of the plugin. */
#define USM_POOL_ALIGN 64

/* Per-worker scratch rows: three rolling hblur buffers (y-1, y, y+1)
 * plus two halo snapshots (rows y_start-1 and y_end) used only for
 * in-place frames — see usm_pool_set_per_frame (SYS-4). */
#define USM_POOL_SCRATCH_ROWS 5

/*
 * Cache-line-aligned worker payload. worker_pool.h stores payloads in an
 * aligned array and requires each slot's size to be a cache-line multiple,
 * so neighboring workers' frame-local state cannot occupy the same line.
 * Thread records, including `seen_gen`, live in the pool's separate aligned
 * thread array. _Alignas(64) both aligns each instance and rounds sizeof up
 * to a 64-byte multiple so aligned_alloc preserves per-element alignment.
 */
typedef struct usm_worker_s {
    /* _Alignas on the first member promotes the whole struct's alignment
     * to 64 and forces sizeof to be a 64-byte multiple, so an aligned
     * array keeps neighboring payload slots from sharing cache lines. C11
     * disallows _Alignas on a typedef name itself, hence placing it here.
     *
     * Threads, the dispatch gate and the exit protocol belong to the
     * shared pool (ARCH-2, worker_pool.h); this struct is pure payload. */

    /* Per-worker constants set at lazy_init. */
    alignas(64) int y_start;
    int        y_end;
    int        width;
    int        height;
    uint8_t   *scratch;       /* 5*width: 3 rolling rows + 2 halo snapshots */

    /* Per-frame state set by main thread before the dispatch. The gate
     * protocol (threading.h) makes these reads race-free: written before
     * the generation bump, observed after the worker re-acquires the
     * gate lock; dst writes flow back through the done barrier. */
    const uint8_t  *src;
    uint8_t        *dst;
    int             src_stride;
    int             dst_stride;
    int             amount_q8;

    /* In-place support (SYS-4): when dst aliases src, a neighbour worker
     * concurrently OVERWRITES the two src rows this worker's boundary
     * hblurs need (y_start-1 and y_end). The main thread snapshots those
     * rows into per-worker scratch before the dispatch; NULL when the
     * frame is not in-place or the row doesn't exist. */
    const uint8_t  *halo_top;   /* pre-frame copy of src row y_start-1 */
    const uint8_t  *halo_bot;   /* pre-frame copy of src row y_end */
} usm_worker_t;

/*
 * Per-stripe flat detection (compile-time opt-in via USM_POOL_FLAT_SKIP).
 *
 * Sum of |src[x+1] - src[x]| over a sampled row inside the stripe. Very low
 * horizontal activity carries little detail USM could enhance, and the
 * combine pass becomes net-loss noise
 * amplification. Skip the combine entirely → identity copy of src→dst for
 * the stripe. Sampling is O(width) versus the full O(width*stripe_height)
 * sweep.
 *
 * Disabled by default because it sacrifices byte-identity with the
 * single-threaded reference up_usm_apply_plane on near-flat-but-not-
 * exactly-flat content (per-pixel delta of up to a few LSB). Enabled
 * for bench tooling (`make bench-flatskip`) where perceptual equivalence
 * is sufficient — that is the call site the feature exists for.
 *
 * The conservative threshold selects only very low horizontal activity such
 * as solid fills, letterbox bars, plain backgrounds, and fades.
 */
#ifndef USM_POOL_FLAT_SKIP
#  define USM_POOL_FLAT_SKIP 0
#endif

#if USM_POOL_FLAT_SKIP
#define USM_FLAT_AVG_DELTA 2

static uint64_t up_usm__row_h_activity(const uint8_t *row, int w)
{
    uint64_t s = 0;
    for (int x = 1; x < w; x++) {
        int d = (int)row[x] - (int)row[x-1];
        s += (uint64_t)(d < 0 ? -d : d);
    }
    return s;
}

/* Identity-copy this worker's stripe (used when the stripe is flat). */
static void usm_worker_copy_stripe(usm_worker_t *w)
{
    for (int y = w->y_start; y < w->y_end; y++) {
        uint8_t       *d = w->dst + (size_t)y * (size_t)w->dst_stride;
        const uint8_t *s = w->src + (size_t)y * (size_t)w->src_stride;
        if (d != s) memcpy(d, s, (size_t)w->width);
    }
}

/* True if the stripe's sampled middle row is visually flat. */
static int usm_worker_stripe_is_flat(const usm_worker_t *w)
{
    int mid_y = w->y_start + (w->y_end - w->y_start) / 2;
    uint64_t act = up_usm__row_h_activity(
        w->src + (size_t)mid_y * (size_t)w->src_stride, w->width);
    return act < (uint64_t)w->width * (uint64_t)USM_FLAT_AVG_DELTA;
}
#endif

struct usm_pool_s {
    int            n_threads;       /* effective count after lazy_init may shrink */
    int            n_threads_pref;  /* user preference, before clamp */
    int            width;
    int            height;

    uint8_t       *scratch;         /* 5*width per worker, contiguous block */

    /* ARCH-2: threads, gate, worker slots, sticky lazy state and the poison
     * flag all live in the shared pool (worker_pool.h). calloc zeroing marks
     * it not-yet-started for destroy. */
    up_worker_pool_t pool;
};

/* The worker slots are pool-owned storage; the payload type is ours. They are
 * contiguous with sizeof(usm_worker_t) stride, so indexing from slot 0 is
 * valid for the whole array. */
static usm_worker_t *usm_workers(usm_pool_t *p)
{
    return (usm_worker_t *)up_worker_pool_slot(&p->pool, 0);
}

/* ===========================================================================
 * Per-dispatch worker work. The pool (worker_pool.h) owns the wake gate, the
 * done barrier and the exit protocol; this file only says what one worker
 * does with one dispatch.
 * =========================================================================*/

/*
 * Fused single-pass sweep over this worker's stripe. Keeps three rolling
 * private hblur row buffers (up = hblur(y-1), mid = hblur(y), dn =
 * hblur(y+1)), combining each row as the window slides down. Boundary rows
 * clamp (y-1 -> 0 at the top, y+1 -> height-1 at the bottom), matching
 * up_usm__pass2_combine exactly.
 */
/* Source row for the hblur reads: the two rows a neighbour stripe may
 * be overwriting concurrently (in-place frames) come from the pre-frame
 * halo snapshots; everything else reads the plane directly. */
static const uint8_t *usm_worker_src_row(const usm_worker_t *w, int y)
{
    if (w->halo_top && y == w->y_start - 1) return w->halo_top;
    if (w->halo_bot && y == w->y_end)       return w->halo_bot;
    return w->src + (size_t)y * (size_t)w->src_stride;
}

static void usm_worker_sweep(usm_worker_t *w)
{
    const int W = w->width;
    const int H = w->height;
    uint8_t *up  = w->scratch;
    uint8_t *mid = w->scratch + (size_t)W;
    uint8_t *dn  = w->scratch + (size_t)2 * (size_t)W;

    int y = w->y_start;
    int y_up = (y > 0) ? (y - 1) : 0;
    up_usm__hblur_row(up,  usm_worker_src_row(w, y_up), W);
    up_usm__hblur_row(mid, usm_worker_src_row(w, y),    W);

    for (; y < w->y_end; y++) {
        int y_dn = (y < H - 1) ? (y + 1) : (H - 1);
        up_usm__hblur_row(dn, usm_worker_src_row(w, y_dn), W);
        up_usm__combine_row(
            w->dst + (size_t)y * (size_t)w->dst_stride,
            w->src + (size_t)y * (size_t)w->src_stride,
            up, mid, dn, W, w->amount_q8);
        uint8_t *t = up; up = mid; mid = dn; dn = t;
    }
}

static void usm_worker_run(usm_worker_t *w)
{
#if USM_POOL_FLAT_SKIP
    if (usm_worker_stripe_is_flat(w)) {
        usm_worker_copy_stripe(w);
        return;
    }
#endif
    usm_worker_sweep(w);
}

/* ===========================================================================
 * Lazy initialization: scratch alloc + worker construction, driven by the
 * shared pool on the first apply() that has work to do (amount_q8 > 0). A
 * partial spawn is fine here — stripes repartition over whatever came up
 * (ops.all_or_nothing = false).
 * =========================================================================*/

/*
 * Total bytes for the shared scratch block: USM_POOL_SCRATCH_ROWS rolling
 * rows of `width` bytes for each of `n` workers. Returns 0 on overflow or
 * invalid input so the caller treats it as an allocation failure.
 */
static size_t usm_pool_scratch_bytes(int n, int width)
{
    if (n <= 0 || width <= 0) return 0;
    size_t per = (size_t)USM_POOL_SCRATCH_ROWS * (size_t)width;
    if (per / (size_t)USM_POOL_SCRATCH_ROWS != (size_t)width) return 0;
    if (per > SIZE_MAX / (size_t)n) return 0;
    return per * (size_t)n;
}

/* Allocate the shared worker scratch block (5*width per worker). Returns
 * 0 on success, -1 on overflow or allocation failure. */
static int usm_pool_alloc_scratch(usm_pool_t *p)
{
    size_t bytes = usm_pool_scratch_bytes(p->n_threads_pref, p->width);
    if (bytes == 0) return -1;
    size_t aligned_bytes =
        (bytes + (USM_POOL_ALIGN - 1)) & ~(size_t)(USM_POOL_ALIGN - 1);
    if (aligned_bytes < bytes) return -1;   /* round-up overflow */
    p->scratch = aligned_alloc(USM_POOL_ALIGN, aligned_bytes);
    return p->scratch ? 0 : -1;
}

/* Pool hook: set up worker slot `i`. Cannot fail — the scratch block is
 * already allocated (usm_pool_prepare) and the stripe bounds are assigned by
 * usm_pool_finalize once the real worker count is known, so a worker cannot
 * read them before the first dispatch bumps the generation. */
static int usm_pool_construct_worker(void *owner, int i)
{
    usm_pool_t *p = (usm_pool_t *)owner;
    usm_worker_t *w = &usm_workers(p)[i];
    w->scratch = p->scratch + (size_t)i
               * (size_t)USM_POOL_SCRATCH_ROWS * (size_t)p->width;
    w->width   = p->width;
    w->height  = p->height;
    return 0;
}

/* Stable final-link symbol for the multiversion ISA regression gate. */
static void USM_ISA_ANCHOR usm_pool_run_worker(void *owner, int i)
{
    usm_worker_run(&usm_workers((usm_pool_t *)owner)[i]);
}

/* Single partitioner (DUP-8): divide `height` into `n` contiguous stripes
 * and write each worker's y_start/y_end. The last stripe absorbs the
 * integer-division remainder so the union of stripes covers [0, height).
 * Called after the spawn loop with the count of workers that actually
 * came up, before any frame can be dispatched to them. */
static void usm_pool_repartition_stripes(usm_worker_t *workers, int n,
                                         int height)
{
    for (int i = 0; i < n; i++) {
        workers[i].y_start = (int)((int64_t)i * height / n);
        workers[i].y_end   = (i == n - 1)
            ? height
            : (int)((int64_t)(i + 1) * height / n);
    }
}

/* Pool hook: one-time setup before any slot exists. */
static int usm_pool_prepare(void *owner)
{
    return usm_pool_alloc_scratch((usm_pool_t *)owner);
}

/* Pool hook: the workers that actually came up (a partial spawn is usable
 * here). Partition the plane across exactly those; unused slots stay zeroed
 * and are never dispatched to. */
static void usm_pool_finalize(void *owner, int n_workers)
{
    usm_pool_t *p = (usm_pool_t *)owner;
    usm_pool_repartition_stripes(usm_workers(p), n_workers, p->height);
    p->n_threads = n_workers;
}

static const up_worker_pool_ops_t usm_pool_ops = {
    .construct      = usm_pool_construct_worker,
    .run            = usm_pool_run_worker,
    .prepare        = usm_pool_prepare,
    .finalize       = usm_pool_finalize,
    .all_or_nothing = false,   /* stripes repartition over a partial spawn */
};

/* ===========================================================================
 * Public API
 * =========================================================================*/

/*
 * Resolve the worker count actually worth spawning for this frame height.
 *
 * PERF-2: the sweep is memory-bandwidth-bound and stops scaling long before
 * the stripe floor runs out of rows. Measured 1080p in-place USM (best of 5):
 * N=1 322 us, N=4 89.8, N=8 58.6, N=10 51.1 (knee), N=12 56.6, N=14 59.3,
 * N=20 60.6, N=30 76.5; 4K plateaus at N=8..12. Past the knee each extra
 * worker only adds gate, barrier and halo-snapshot cost, so the auto policy's
 * 14 threads on a 32-core box (+16%) and 30 on a 64-core box (+50%) are pure
 * loss. Cap the default policy at the plateau.
 *
 * An explicit stripe_min_rows means the caller has taken over the partition
 * policy (--autoupscale-usm-stripe-min-rows) — respect it and skip the cap.
 */
static int usm_pool_resolve_threads(int n_threads, int height,
                                    int stripe_min_rows)
{
    /* Cap at the pool-wide maximum here, not only in up_threads_decide:
     * a direct API caller with a huge n_threads and a tall frame would
     * otherwise size the worker array from an unchecked multiply. */
    if (n_threads > UP_THREADS_MAX) n_threads = UP_THREADS_MAX;

    if (stripe_min_rows <= 0) {
        if (n_threads > USM_MAX_USEFUL_THREADS)
            n_threads = USM_MAX_USEFUL_THREADS;
        stripe_min_rows = USM_STRIPE_MIN_ROWS;
    }

    /* Each stripe at least stripe_min_rows rows tall. */
    int max_by_size = height / stripe_min_rows;
    if (max_by_size < 1) max_by_size = 1;
    return n_threads > max_by_size ? max_by_size : n_threads;
}

usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows)
{
    if (n_threads < 1 || width <= 0 || height <= 0) return NULL;

    n_threads = usm_pool_resolve_threads(n_threads, height, stripe_min_rows);

    usm_pool_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->n_threads_pref = n_threads;
    p->n_threads      = n_threads;  /* updated by the finalize hook if it shrinks */
    p->width          = width;
    p->height         = height;
    up_worker_pool_config(&p->pool, &usm_pool_ops, p, n_threads,
                          sizeof(usm_worker_t));
    return p;
}

/* In-place frames only (SYS-4): snapshot the two src rows this worker's
 * boundary hblurs need but a neighbour worker concurrently overwrites
 * (y_start-1 belongs to worker i-1's stripe, y_end to worker i+1's).
 * Serial main-thread work, two rows per worker, before dispatch. */
static void usm_worker_snapshot_halo(usm_worker_t *w, const uint8_t *src,
                                     int src_stride, int height)
{
    uint8_t *top = w->scratch + (size_t)3 * (size_t)w->width;
    uint8_t *bot = w->scratch + (size_t)4 * (size_t)w->width;
    if (w->y_start > 0) {
        memcpy(top, src + (size_t)(w->y_start - 1) * (size_t)src_stride,
               (size_t)w->width);
        w->halo_top = top;
    }
    if (w->y_end < height) {
        memcpy(bot, src + (size_t)w->y_end * (size_t)src_stride,
               (size_t)w->width);
        w->halo_bot = bot;
    }
}

/*
 * Update each worker's per-frame state to point at the current src/dst
 * buffers and amount. Called by the main thread while workers are
 * blocked on the go gate - no synchronization needed (the gate's mutex
 * in up_worker_pool_dispatch publishes these writes; see usm_worker_s).
 */
static void usm_pool_set_per_frame(usm_pool_t *p,
                                   uint8_t *dst, int dst_stride,
                                   const uint8_t *src, int src_stride,
                                   int amount_q8)
{
    for (int i = 0; i < p->n_threads; i++) {
        usm_worker_t *w = &usm_workers(p)[i];
        w->src        = src;
        w->src_stride = src_stride;
        w->dst        = dst;
        w->dst_stride = dst_stride;
        w->amount_q8  = amount_q8;
        w->halo_top   = NULL;
        w->halo_bot   = NULL;
        if (dst == src)
            usm_worker_snapshot_halo(w, src, src_stride, p->height);
    }
}

static int usm_pool_validate_args(const usm_pool_t *p,
                                  const uint8_t *dst, int dst_stride,
                                  const uint8_t *src, int src_stride)
{
    if (!p) return -1;
    if (!up_usm__plane_args_ok(dst, dst_stride, src, src_stride, p->width))
        return -1;
    /* In-place means EXACT aliasing: same base, same stride. A stride
     * mismatch on the same base would interleave reads and writes of
     * different rows — reject rather than corrupt. */
    if (dst == src && dst_stride != src_stride) return -1;
    return 0;
}

int up_usm_pool_apply(usm_pool_t *p,
                      uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      int amount_q8)
{
    if (usm_pool_validate_args(p, dst, dst_stride, src, src_stride) != 0)
        return UP_USM_APPLY_FAILED_UNCHANGED;
    if (up_worker_pool_broken(&p->pool))
        return UP_USM_APPLY_FAILED_UNCHANGED;

    amount_q8 = up_usm__clamp_amount_q8(amount_q8);

    /* Identity fast path: no thread activity, no scratch alloc. */
    if (amount_q8 == 0) {
        up_usm__apply_identity(dst, dst_stride, src, src_stride,
                               p->width, p->height);
        return UP_USM_APPLY_OK;
    }

    if (up_worker_pool_ensure_started(&p->pool) != 0)
        return UP_USM_APPLY_FAILED_UNCHANGED;

    usm_pool_set_per_frame(p, dst, dst_stride, src, src_stride, amount_q8);
    return up_worker_pool_dispatch(&p->pool) == 0
        ? UP_USM_APPLY_OK : UP_USM_APPLY_OUTPUT_UNCERTAIN;
}

int up_usm_pool_effective_threads(const usm_pool_t *p)
{
    return p ? p->n_threads : 0;
}

void up_usm_pool_destroy(usm_pool_t *p)
{
    if (!p) return;

    up_worker_pool_destroy(&p->pool);
    free(p->scratch);
    free(p);
}
