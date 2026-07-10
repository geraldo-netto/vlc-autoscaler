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
 * - The 3-row scratch is one pool-level allocation of 3*width bytes per
 *   worker; far smaller than the old width*height workspace and the only
 *   allocation that can fail in lazy init (sticky on failure).
 *
 * - The thread pool is lazy: created on the first apply() call so that
 *   probing-only Open/Close cycles (chain solver) cost nothing.
 *
 * - amount_q8 == 0 short-circuits to an identity copy with no thread
 *   activity. This matches up_usm_apply_plane's behavior and keeps the
 *   pool cheap when USM is configured off.
 *****************************************************************************/

#include "usm_pool.h"
#include "usm.h"
#include "threading.h"

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
#else
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

#include <pthread.h>
#include <semaphore.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Each stripe at least this many rows tall; smaller stripes are
 * dominated by kernel boundary handling and not worth threading. */
#define USM_STRIPE_MIN_ROWS 8

/* Workspace alignment - matches the rest of the plugin. */
#define USM_POOL_ALIGN 64

/* Per-worker scratch rows: three rolling hblur buffers (y-1, y, y+1)
 * plus two halo snapshots (rows y_start-1 and y_end) used only for
 * in-place frames — see usm_pool_set_per_frame (SYS-4). */
#define USM_POOL_SCRATCH_ROWS 5

/*
 * Cache-line-aligned to prevent false sharing between adjacent workers.
 * Each worker writes its own `seen_gen` on every dispatch and its private
 * scratch rows; without padding, two workers whose structs share a
 * 64-byte cache line would invalidate each other's lines on every frame.
 * _Alignas(64) both aligns each instance AND rounds sizeof up to a
 * 64-byte multiple so an aligned_alloc'd array keeps the per-element
 * alignment.
 */
typedef struct usm_worker_s {
    /* _Alignas on the first member promotes the whole struct's alignment
     * to 64 and forces sizeof to be a 64-byte multiple, so an aligned
     * array (one element per cache line) keeps every per-worker write
     * off neighboring workers' cache lines. C11 disallows _Alignas on a
     * typedef name itself, hence placing it here. */
    alignas(64) pthread_t  thread;
    bool       thread_started;
    bool       should_exit;   /* finish unseen generation, then exit */

    /* Dispatch gate, shared across workers and owned by the pool — the
     * same design as the zimg pool (SCAL-2): the main thread bumps
     * *generation under *go_lock and wakes everyone with ONE
     * pthread_cond broadcast; each worker sleeps until *generation
     * advances past its own seen_gen. Completion is a counting barrier:
     * each worker decrements *pending after its stripe; the one that
     * drives it to zero posts *all_done, which the main thread waits on
     * exactly once. */
    pthread_mutex_t *go_lock;
    pthread_cond_t  *go_cv;
    uint64_t        *generation;   /* shared, guarded by *go_lock */
    uint64_t         seen_gen;
    atomic_int      *pending;
    sem_t           *all_done;

    /* Per-worker constants set at lazy_init. */
    int        y_start, y_end;
    int        width, height;
    uint8_t   *scratch;       /* 3*width private rolling rows, owned by pool */

    /* Per-frame state set by main thread before the dispatch.
     *
     * The fence that makes these reads race-free is the go gate: the
     * main thread writes every field below, then bumps *generation under
     * *go_lock (the unlock releases); the worker re-acquires *go_lock to
     * observe the new generation before reading them. On the done side
     * the worker's acq_rel fetch_sub on *pending plus the
     * sem_post/sem_wait on *all_done publish its dst writes back to the
     * main thread before the next frame's set_per_frame. */
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
 * Sum of |src[x+1] - src[x]| over a single sampled row inside the stripe.
 * Below ~2 average per-pixel delta the stripe carries no detail USM
 * could enhance, and the combine pass becomes net-loss noise
 * amplification. Skip the combine entirely → identity copy of src→dst for
 * the stripe. Cost: O(width) per stripe vs O(width*stripe_height) for the
 * full sweep, so even at 0% skip rate the overhead is <2% of the sweep.
 *
 * Disabled by default because it sacrifices byte-identity with the
 * single-threaded reference up_usm_apply_plane on near-flat-but-not-
 * exactly-flat content (per-pixel delta of up to a few LSB). Enabled
 * for bench tooling (`make bench-flatskip`) where perceptual equivalence
 * is sufficient — that is the call site the feature exists for.
 *
 * Conservative threshold: USM_FLAT_AVG_DELTA=2 means avg neighbour diff
 * < 2/255 ≈ 0.8%. Real video almost never hits this except for solid
 * colour fills (letterbox bars, plain backgrounds, fade-to-black).
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
    int            width, height;

    usm_worker_t  *workers;

    /* SCAL-2-style wake gate + counting done-barrier (see usm_worker_s). */
    pthread_mutex_t go_lock;
    pthread_cond_t  go_cv;
    uint64_t        generation;     /* bumped per dispatch under go_lock */
    bool            go_gate_inited; /* destroy guard: mutex+cond init'd */
    atomic_int      pending;        /* live workers this dispatch */
    sem_t           all_done;       /* posted once when pending hits 0 */
    bool            all_done_inited;

    uint8_t       *scratch;         /* 3*width per worker, contiguous block */

    bool           lazy_init_done;
    bool           lazy_init_failed;
    bool           pool_broken;
};

/* ===========================================================================
 * Worker thread main loop. Receives work via the shared generation gate
 * (one broadcast per dispatch) and signals completion through the pending
 * counting barrier. Exits cleanly when the main thread sets should_exit
 * under go_lock and broadcasts.
 * =========================================================================*/

/*
 * Fused single-pass sweep over this worker's stripe. Keeps three rolling
 * private hblur row buffers (up = hblur(y-1), mid = hblur(y), dn =
 * hblur(y+1)), combining each row as the window slides down. Boundary rows
 * clamp (y-1 -> 0 at the top, y+1 -> height-1 at the bottom), matching
 * up_usm__pass2_combine exactly. CCN 4.
 */
/* Source row for the hblur reads: the two rows a neighbour stripe may
 * be overwriting concurrently (in-place frames) come from the pre-frame
 * halo snapshots; everything else reads the plane directly. CCN 3. */
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

/* Block until the main thread bumps *generation (new dispatch) or sets
 * should_exit. An unseen dispatch is completed before exit so fatal barrier
 * recovery can join without leaving a partly-written frame. */
static bool usm_worker_wait_for_go(usm_worker_t *w)
{
    pthread_mutex_lock(w->go_lock);
    while (*w->generation == w->seen_gen && !w->should_exit)
        pthread_cond_wait(w->go_cv, w->go_lock);
    bool run = *w->generation != w->seen_gen;
    w->seen_gen = *w->generation;
    pthread_mutex_unlock(w->go_lock);
    return run;
}

static void *usm_worker_main(void *arg)
{
    usm_worker_t *w = (usm_worker_t *)arg;
    for (;;) {
        if (!usm_worker_wait_for_go(w)) break;
        usm_worker_run(w);
        /* Last worker to finish posts all_done exactly once. acq_rel so
         * the dst writes above join the release sequence on *pending. */
        if (atomic_fetch_sub_explicit(w->pending, 1, memory_order_acq_rel) == 1)
            sem_post(w->all_done);
    }
    return NULL;
}

/* ===========================================================================
 * Lazy initialization: scratch alloc + worker spawn. Called on the first
 * apply() invocation that actually has work to do (amount_q8 > 0). Returns
 * 0 on success, -1 on any allocation/spawn failure (caller sets the sticky
 * lazy_init_failed flag in that case). On partial failure (some workers
 * spawned but not all), n_threads is shrunk to the actually-spawned count
 * and we proceed - the partition logic handles uneven counts.
 * =========================================================================*/
static int usm_pool_init_gate(usm_pool_t *p)
{
    if (pthread_mutex_init(&p->go_lock, NULL) != 0) return -1;
    if (pthread_cond_init(&p->go_cv, NULL) != 0) { pthread_mutex_destroy(&p->go_lock); return -1; }
    p->go_gate_inited = true;
    if (sem_init(&p->all_done, 0, 0) != 0) return -1;
    p->all_done_inited = true;
    return 0;
}

/*
 * Total bytes for the shared scratch block: USM_POOL_SCRATCH_ROWS rolling
 * rows of `width` bytes for each of `n` workers. Returns 0 on overflow or
 * invalid input so the caller treats it as an allocation failure. CCN 3.
 */
static size_t usm_pool_scratch_bytes(int n, int width)
{
    if (n <= 0 || width <= 0) return 0;
    size_t per = (size_t)USM_POOL_SCRATCH_ROWS * (size_t)width;
    if (per / (size_t)USM_POOL_SCRATCH_ROWS != (size_t)width) return 0;
    if (per > SIZE_MAX / (size_t)n) return 0;
    return per * (size_t)n;
}

/* Allocate the shared worker scratch block (3*width per worker). Returns
 * 0 on success, -1 on overflow or allocation failure. CCN 3. */
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

static int usm_pool_spawn_worker(usm_pool_t *p, int i, int n)
{
    usm_worker_t *w = &p->workers[i];
    w->go_lock    = &p->go_lock;
    w->go_cv      = &p->go_cv;
    w->generation = &p->generation;
    w->seen_gen   = p->generation;  /* don't run a frame before the first dispatch */
    w->pending    = &p->pending;
    w->all_done   = &p->all_done;
    w->scratch   = p->scratch + (size_t)i
                 * (size_t)USM_POOL_SCRATCH_ROWS * (size_t)p->width;
    w->width     = p->width;
    w->height    = p->height;
    w->y_start   = (int)((int64_t)i * p->height / n);
    w->y_end     = (i == n - 1)
        ? p->height
        : (int)((int64_t)(i + 1) * p->height / n);

    if (pthread_create(&w->thread, NULL, usm_worker_main, w) != 0)
        return -1;
    w->thread_started = true;
    return 0;
}

/* Divide `height` into `n` contiguous stripes and write each worker's
 * y_start/y_end. The last stripe absorbs the integer-division remainder
 * so the union of stripes covers [0, height). Called unconditionally
 * after the spawn loop: when every worker spawned this is a redundant
 * (idempotent) re-assignment, but when only `constructed < n_pref`
 * workers came up it is the single place that re-balances them. Keeping
 * the call unconditional means every test path exercises this helper. */
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

/* Allocate the worker array (one cache-line-aligned slot each) and zero
 * it. Returns 0 on success, -1 on failure. CCN 2. */
static int usm_pool_alloc_workers(usm_pool_t *p)
{
    /* aligned_alloc not calloc: usm_worker_t carries _Alignas(64) so each
     * element sits on its own cache line; calloc returns malloc-default
     * (16) alignment which would defeat the layout. sizeof is already a
     * multiple of 64 thanks to _Alignas, satisfying aligned_alloc's C11
     * size constraint. Manual memset replaces calloc's zero-init. */
    size_t total = (size_t)p->n_threads_pref * sizeof(*p->workers);
    p->workers = aligned_alloc(64, total);
    if (!p->workers) return -1;
    memset(p->workers, 0, total);
    return 0;
}

static int usm_pool_spawn_all(usm_pool_t *p)
{
    int constructed = 0;
    for (int i = 0; i < p->n_threads_pref; i++) {
        if (usm_pool_spawn_worker(p, i, p->n_threads_pref) != 0) break;
        constructed++;
    }
    if (constructed == 0) return -1;

    /* Repartition unconditionally. When constructed == n_threads_pref
     * this is idempotent (same math the spawn loop just used). When
     * constructed < n_threads_pref it re-balances stripes across the
     * actually-spawned workers; the unused slots stay zeroed and are
     * skipped by destroy. */
    usm_pool_repartition_stripes(p->workers, constructed, p->height);
    p->n_threads = constructed;
    return 0;
}

static int usm_pool_lazy_init(usm_pool_t *p)
{
    if (usm_pool_alloc_scratch(p) != 0) return -1;
    if (usm_pool_alloc_workers(p) != 0) return -1;
    if (usm_pool_init_gate(p) != 0) return -1;
    return usm_pool_spawn_all(p);
}

/* ===========================================================================
 * Public API
 * =========================================================================*/

usm_pool_t *up_usm_pool_create(int n_threads, int width, int height,
                               int stripe_min_rows)
{
    if (n_threads < 1 || width <= 0 || height <= 0) return NULL;

    /* stripe_min_rows <= 0 → use compile-time default. */
    if (stripe_min_rows <= 0) stripe_min_rows = USM_STRIPE_MIN_ROWS;

    /* Each stripe at least stripe_min_rows rows tall. */
    int max_by_size = height / stripe_min_rows;
    if (max_by_size < 1) max_by_size = 1;
    if (n_threads > max_by_size) n_threads = max_by_size;

    usm_pool_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->n_threads_pref = n_threads;
    p->n_threads      = n_threads;  /* updated by lazy_init if it shrinks */
    p->width          = width;
    p->height         = height;
    return p;
}

/* In-place frames only (SYS-4): snapshot the two src rows this worker's
 * boundary hblurs need but a neighbour worker concurrently overwrites
 * (y_start-1 belongs to worker i-1's stripe, y_end to worker i+1's).
 * Serial main-thread work, two rows per worker, before dispatch. CCN 3. */
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
 * in usm_pool_run publishes these writes; see usm_worker_s).
 */
static void usm_pool_set_per_frame(usm_pool_t *p,
                                   uint8_t *dst, int dst_stride,
                                   const uint8_t *src, int src_stride,
                                   int amount_q8)
{
    for (int i = 0; i < p->n_threads; i++) {
        usm_worker_t *w = &p->workers[i];
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

/*
 * Dispatch the (single) fused sweep to all workers and wait for all to
 * finish — O(1) syscalls each way (SYS-1, mirrors the zimg pool's
 * SCAL-2 design): arm the done-barrier, bump the generation once and
 * wake every worker with a single broadcast; then one wait on the
 * counting barrier's sem, posted by the last worker to finish. Returns
 * 0 once every worker completed. On a non-EINTR barrier failure, drains the
 * dispatched generation, joins the pool, and returns -1 with a sticky fatal
 * state.
 */
static void usm_pool_stop_workers(usm_pool_t *p);

static int usm_pool_run(usm_pool_t *p)
{
    pthread_mutex_lock(&p->go_lock);
    atomic_store_explicit(&p->pending, p->n_threads, memory_order_relaxed);
    p->generation++;
    pthread_cond_broadcast(&p->go_cv);
    pthread_mutex_unlock(&p->go_lock);

    if (up_sem_wait_nointr(&p->all_done) == 0) return 0;
    p->pool_broken = true;
    usm_pool_stop_workers(p);
    return -1;
}

static int usm_pool_validate_args(const usm_pool_t *p,
                                  const uint8_t *dst, int dst_stride,
                                  const uint8_t *src, int src_stride)
{
    if (!p) return -1;
    if (!dst || !src) return -1;
    if (dst_stride < p->width || src_stride < p->width) return -1;
    /* In-place means EXACT aliasing: same base, same stride. A stride
     * mismatch on the same base would interleave reads and writes of
     * different rows — reject rather than corrupt. */
    if (dst == src && dst_stride != src_stride) return -1;
    return 0;
}

/* Lazy init on first real call. Returns 0 on success (already done or
 * just succeeded), -1 on prior or fresh failure (sticky). */
static int usm_pool_ensure_init(usm_pool_t *p)
{
    if (p->lazy_init_done) return 0;
    if (p->lazy_init_failed) return -1;
    if (usm_pool_lazy_init(p) != 0) {
        p->lazy_init_failed = true;
        return -1;
    }
    p->lazy_init_done = true;
    return 0;
}

int up_usm_pool_apply(usm_pool_t *p,
                      uint8_t *dst, int dst_stride,
                      const uint8_t *src, int src_stride,
                      int amount_q8)
{
    if (usm_pool_validate_args(p, dst, dst_stride, src, src_stride) != 0)
        return -1;
    if (p->pool_broken) return -1;

    amount_q8 = up_usm__clamp_amount_q8(amount_q8);

    /* Identity fast path: no thread activity, no scratch alloc. */
    if (amount_q8 == 0) {
        up_usm__apply_identity(dst, dst_stride, src, src_stride,
                               p->width, p->height);
        return 0;
    }

    if (usm_pool_ensure_init(p) != 0) return -1;

    usm_pool_set_per_frame(p, dst, dst_stride, src, src_stride, amount_q8);
    return usm_pool_run(p);
}

/* Tell every started worker to finish any unseen generation, then exit. */
static void usm_pool_wake_all_for_exit(usm_pool_t *p)
{
    /* If the gate never initialized, no thread was ever spawned (spawn
     * runs after gate init), so there is nothing to wake. */
    if (!p->go_gate_inited) return;
    pthread_mutex_lock(&p->go_lock);
    for (int i = 0; i < p->n_threads; i++)
        if (p->workers[i].thread_started)
            p->workers[i].should_exit = true;
    pthread_cond_broadcast(&p->go_cv);
    pthread_mutex_unlock(&p->go_lock);
}

static void usm_pool_stop_workers(usm_pool_t *p)
{
    if (!p->workers) return;
    usm_pool_wake_all_for_exit(p);
    for (int i = 0; i < p->n_threads; i++) {
        if (!p->workers[i].thread_started) continue;
        pthread_join(p->workers[i].thread, NULL);
        p->workers[i].thread_started = false;
    }
}

void up_usm_pool_destroy(usm_pool_t *p)
{
    if (!p) return;

    if (p->workers) {
        usm_pool_stop_workers(p);
        free(p->workers);
    }
    if (p->all_done_inited) sem_destroy(&p->all_done);
    if (p->go_gate_inited) {
        pthread_cond_destroy(&p->go_cv);
        pthread_mutex_destroy(&p->go_lock);
    }
    free(p->scratch);
    free(p);
}
