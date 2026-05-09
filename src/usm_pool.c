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
 * - The shared workspace is sized width * height bytes. It's overwritten
 *   in pass 1 by all workers (each writing only its own rows) and read
 *   in pass 2 by all workers (each reading three rows centered on its
 *   own; the topmost and bottommost rows clamp at workspace edges).
 *
 * - Pass-1-then-pass-2 ordering is enforced by the main thread waiting
 *   on all N pass-1 done signals before sending pass-2 go signals.
 *   No barrier primitive needed; semaphores already serialize.
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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Each stripe at least this many rows tall; smaller stripes are
 * dominated by kernel boundary handling and not worth threading. */
#define USM_STRIPE_MIN_ROWS 8

/* Workspace alignment - matches the rest of the plugin. */
#define USM_POOL_ALIGN 64

/*
 * Cache-line-aligned to prevent false sharing between adjacent workers.
 * Each worker writes to its own `phase` and `sem_t.go.value` on every
 * dispatch; without padding, two workers whose structs share a 64-byte
 * cache line would invalidate each other's lines on every frame, costing
 * ~10s of ns/frame per worker pair. _Alignas(64) both aligns each instance
 * AND rounds sizeof up to a 64-byte multiple so an aligned_alloc'd array
 * keeps the per-element alignment.
 */
typedef struct usm_worker_s {
    /* _Alignas on the first member promotes the whole struct's alignment
     * to 64 and forces sizeof to be a 64-byte multiple, so an aligned
     * array (one element per cache line) keeps every per-worker write
     * off neighboring workers' cache lines. C11 disallows _Alignas on a
     * typedef name itself, hence placing it here. */
    _Alignas(64) pthread_t  thread;
    sem_t      go;
    sem_t     *done;          /* shared, owned by pool */
    bool       thread_started;
    bool       go_inited;
    bool       should_exit;

    /* Per-worker constants set at lazy_init. */
    int        y_start, y_end;
    int        width, height;
    uint8_t   *workspace;     /* shared buffer, owned by pool */

    /* Per-frame state set by main thread before sem_post(go). */
    int             phase;    /* 0 = hblur (pass 1), 1 = combine (pass 2) */
    const uint8_t  *src;
    uint8_t        *dst;
    int             src_stride;
    int             dst_stride;
    int             amount_q8;

    /* Set by phase 0; consumed by phase 1.
     * 1 = stripe is visually flat → phase 2 short-circuits to identity.
     * Only meaningful when USM_POOL_FLAT_SKIP is enabled. */
    int             flat_skip;
} usm_worker_t;

/*
 * Per-stripe flat detection (compile-time opt-in via USM_POOL_FLAT_SKIP).
 *
 * Sum of |src[x+1] - src[x]| over a single sampled row inside the stripe.
 * Below ~2 average per-pixel delta the stripe carries no detail USM
 * could enhance, and combine_row's pass becomes net-loss noise
 * amplification. Skip combine entirely → identity copy of src→dst for
 * the stripe. Cost: O(width) per stripe vs O(width*stripe_height) for
 * combine, so even at 0% skip rate the overhead is <2% of phase 2.
 *
 * Disabled by default because it sacrifices byte-identity with the
 * single-threaded reference up_usm_apply_plane on near-flat-but-not-
 * exactly-flat content (per-pixel delta of up to a few LSB). Enabled
 * for bench tooling where perceptual equivalence is sufficient.
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
#endif

struct usm_pool_s {
    int            n_threads;       /* effective count after lazy_init may shrink */
    int            n_threads_pref;  /* user preference, before clamp */
    int            width, height;

    usm_worker_t  *workers;
    sem_t          done;
    bool           done_inited;
    uint8_t       *workspace;

    bool           lazy_init_done;
    bool           lazy_init_failed;
};

/* ===========================================================================
 * Worker thread main loop. Receives work via sem_post(&w->go) and signals
 * completion via sem_post(w->done). Phases distinguished by w->phase.
 * Exits cleanly when the main thread sets should_exit = true and posts go.
 * =========================================================================*/

/* Pass 1: horizontal blur every row in our stripe into the shared
 * workspace. No reads of other workers' rows; no race because each
 * worker writes a disjoint row range. With USM_POOL_FLAT_SKIP, also
 * sample the middle row of the stripe to decide whether phase 2 can
 * short-circuit. */
static void usm_worker_phase0_hblur(usm_worker_t *w)
{
    for (int y = w->y_start; y < w->y_end; y++) {
        up_usm__hblur_row(
            w->workspace + (size_t)y * (size_t)w->width,
            w->src + (size_t)y * (size_t)w->src_stride,
            w->width);
    }

#if USM_POOL_FLAT_SKIP
    {
        int mid_y = w->y_start + (w->y_end - w->y_start) / 2;
        uint64_t act = up_usm__row_h_activity(
            w->src + (size_t)mid_y * (size_t)w->src_stride,
            w->width);
        w->flat_skip =
            (act < (uint64_t)w->width * (uint64_t)USM_FLAT_AVG_DELTA);
    }
#endif
}

/* Stripe was flat in phase 0 → identity copy is bit-identical within
 * rounding to combine output, since combine adds (src - hblur)*amount
 * where (src - hblur) ≈ 0 on flat areas. Saves one O(width*stripe_h)
 * pass of combine kernel. */
static void usm_worker_phase1_skip_copy(usm_worker_t *w)
{
    for (int y = w->y_start; y < w->y_end; y++) {
        if (w->dst + (size_t)y * (size_t)w->dst_stride
            != w->src + (size_t)y * (size_t)w->src_stride)
            memcpy(
                w->dst + (size_t)y * (size_t)w->dst_stride,
                w->src + (size_t)y * (size_t)w->src_stride,
                (size_t)w->width);
    }
}

/* Pass 2: combine workspace[y-1, y, y+1] with src to produce dst, on
 * each row in our stripe. Reads of workspace rows just above y_start
 * and just below y_end-1 belong to the neighboring workers but are
 * race-free because pass 1 fully completed before any pass 2 work
 * began. */
static void usm_worker_phase1_combine(usm_worker_t *w)
{
    for (int y = w->y_start; y < w->y_end; y++) {
        int yu = (y > 0) ? (y - 1) : 0;
        int yd = (y < w->height - 1) ? (y + 1) : (w->height - 1);
        up_usm__combine_row(
            w->dst + (size_t)y * (size_t)w->dst_stride,
            w->src + (size_t)y * (size_t)w->src_stride,
            w->workspace + (size_t)yu * (size_t)w->width,
            w->workspace + (size_t)y  * (size_t)w->width,
            w->workspace + (size_t)yd * (size_t)w->width,
            w->width, w->amount_q8);
    }
}

static void *usm_worker_main(void *arg)
{
    usm_worker_t *w = (usm_worker_t *)arg;
    for (;;) {
        sem_wait(&w->go);
        if (w->should_exit) break;

        if (w->phase == 0)
            usm_worker_phase0_hblur(w);
        else if (USM_POOL_FLAT_SKIP && w->flat_skip)
            usm_worker_phase1_skip_copy(w);
        else
            usm_worker_phase1_combine(w);

        sem_post(w->done);
    }
    return NULL;
}

/* ===========================================================================
 * Lazy initialization: workspace alloc + worker spawn. Called on the first
 * apply() invocation that actually has work to do (amount_q8 > 0). Returns
 * 0 on success, -1 on any allocation/spawn failure (caller sets the sticky
 * lazy_init_failed flag in that case). On partial failure (some workers
 * spawned but not all), n_threads is shrunk to the actually-spawned count
 * and we proceed - the partition logic handles uneven counts.
 * =========================================================================*/
static int usm_pool_init_done_sem(usm_pool_t *p)
{
    if (sem_init(&p->done, 0, 0) != 0) return -1;
    p->done_inited = true;
    return 0;
}

static int usm_pool_spawn_worker(usm_pool_t *p, int i, int n)
{
    usm_worker_t *w = &p->workers[i];
    w->done      = &p->done;
    w->workspace = p->workspace;
    w->width     = p->width;
    w->height    = p->height;
    w->y_start   = (int)((int64_t)i * p->height / n);
    w->y_end     = (i == n - 1)
        ? p->height
        : (int)((int64_t)(i + 1) * p->height / n);

    if (sem_init(&w->go, 0, 0) != 0) return -1;
    w->go_inited = true;

    if (pthread_create(&w->thread, NULL, usm_worker_main, w) != 0)
        return -1;
    w->thread_started = true;
    return 0;
}

static int usm_pool_lazy_init(usm_pool_t *p)
{
    /* aligned_alloc requires size to be a multiple of alignment per
     * C11 (glibc relaxes this; ASan does not). Round up. */
    size_t plane_bytes = (size_t)p->width * (size_t)p->height;
    size_t aligned_bytes =
        (plane_bytes + (USM_POOL_ALIGN - 1)) & ~(size_t)(USM_POOL_ALIGN - 1);
    p->workspace = aligned_alloc(USM_POOL_ALIGN, aligned_bytes);
    if (!p->workspace) return -1;

    /* aligned_alloc not calloc: usm_worker_t carries _Alignas(64) so each
     * element sits on its own cache line; calloc returns malloc-default
     * (16) alignment which would defeat the layout. sizeof is already a
     * multiple of 64 thanks to _Alignas, satisfying aligned_alloc's C11
     * size constraint. Manual memset replaces calloc's zero-init. */
    {
        size_t total = (size_t)p->n_threads_pref * sizeof(*p->workers);
        p->workers = aligned_alloc(64, total);
        if (!p->workers) return -1;
        memset(p->workers, 0, total);
    }

    if (usm_pool_init_done_sem(p) != 0) return -1;

    int constructed = 0;
    for (int i = 0; i < p->n_threads_pref; i++) {
        if (usm_pool_spawn_worker(p, i, p->n_threads_pref) != 0) break;
        constructed++;
    }
    if (constructed == 0) return -1;

    /* If we got fewer workers than requested, repartition the stripes
     * across only the constructed ones. The unused worker slots stay
     * zeroed (calloc) and are skipped by destroy. */
    if (constructed < p->n_threads_pref) {
        for (int i = 0; i < constructed; i++) {
            usm_worker_t *w = &p->workers[i];
            w->y_start = (int)((int64_t)i * p->height / constructed);
            w->y_end   = (i == constructed - 1)
                ? p->height
                : (int)((int64_t)(i + 1) * p->height / constructed);
        }
    }
    p->n_threads = constructed;
    return 0;
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

/*
 * Identity-copy fast path for amount_q8 == 0. Mirrors the matching
 * branch in up_usm_apply_plane (single-threaded). No thread activity.
 */
static void usm_pool_identity(uint8_t *dst, int dst_stride,
                              const uint8_t *src, int src_stride,
                              int width, int height)
{
    if (dst == src && dst_stride == src_stride) return;
    /* Unified-stride fast path: see comment in up_usm__apply_identity
     * (src/usm.h). Collapses N memcpys into 1 when the whole plane is
     * contiguous in both buffers. */
    if (dst_stride == src_stride && src_stride == width) {
        memcpy(dst, src, (size_t)width * (size_t)height);
        return;
    }
    for (int y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * (size_t)dst_stride,
               src + (size_t)y * (size_t)src_stride,
               (size_t)width);
    }
}

/*
 * Update each worker's per-frame state to point at the current src/dst
 * buffers and amount. Called by the main thread while workers are
 * blocked on their `go` semaphore - no synchronization needed.
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
    }
}

/*
 * Dispatch one phase to all workers and wait for all to finish.
 * Returns after the N-th sem_wait(&done), which means every worker
 * has completed its row range for the requested phase.
 */
static void usm_pool_run_phase(usm_pool_t *p, int phase)
{
    for (int i = 0; i < p->n_threads; i++) {
        p->workers[i].phase = phase;
        sem_post(&p->workers[i].go);
    }
    for (int i = 0; i < p->n_threads; i++)
        sem_wait(&p->done);
}

static int usm_pool_validate_args(const usm_pool_t *p,
                                  const uint8_t *dst, int dst_stride,
                                  const uint8_t *src, int src_stride)
{
    if (!p) return -1;
    if (!dst || !src) return -1;
    if (dst_stride < p->width || src_stride < p->width) return -1;
    return 0;
}

/* Clamp amount, matching up_usm_apply_plane. */
static int usm_pool_clamp_amount(int amount_q8)
{
    if (amount_q8 < 0) return 0;
    if (amount_q8 > UP_USM_AMOUNT_Q8_MAX) return UP_USM_AMOUNT_Q8_MAX;
    return amount_q8;
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

    amount_q8 = usm_pool_clamp_amount(amount_q8);

    /* Identity fast path: no thread activity, no workspace alloc. */
    if (amount_q8 == 0) {
        usm_pool_identity(dst, dst_stride, src, src_stride,
                          p->width, p->height);
        return 0;
    }

    if (usm_pool_ensure_init(p) != 0) return -1;

    usm_pool_set_per_frame(p, dst, dst_stride, src, src_stride, amount_q8);
    usm_pool_run_phase(p, 0);  /* pass 1: hblur into workspace */
    usm_pool_run_phase(p, 1);  /* pass 2: combine workspace + src -> dst */
    return 0;
}

void up_usm_pool_destroy(usm_pool_t *p)
{
    if (!p) return;

    if (p->workers) {
        /* Signal all started threads to exit. Workers that never
         * started (partial init) have thread_started == false. */
        for (int i = 0; i < p->n_threads; i++) {
            if (p->workers[i].thread_started) {
                p->workers[i].should_exit = true;
                sem_post(&p->workers[i].go);
            }
        }
        for (int i = 0; i < p->n_threads; i++) {
            if (p->workers[i].thread_started)
                pthread_join(p->workers[i].thread, NULL);
            if (p->workers[i].go_inited)
                sem_destroy(&p->workers[i].go);
        }
        free(p->workers);
    }
    if (p->done_inited) sem_destroy(&p->done);
    free(p->workspace);
    free(p);
}
