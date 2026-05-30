// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_zimg.c - zimg backend with slice-threaded resampling
 *****************************************************************************
 * Higher-quality alternative to swscale (Spline36 + tighter rounding) plus
 * slice threading: the output frame is split into N horizontal stripes and
 * processed in parallel by a persistent worker pool.
 *
 * COPY-IN / COPY-OUT
 *
 * The plugin maintains persistent, page-aligned scratch buffers (source
 * + destination, sized for the current stream). On each Filter() call:
 *   1. memcpy VLC's source picture -> scratch source buffer
 *   2. Dispatch threaded zimg on the scratch buffers
 *   3. memcpy scratch destination -> VLC's destination picture
 *
 * The copies cost a few hundred MB/s of memory bandwidth (negligible vs
 * main memory's 50+ GB/s on modern systems) and buy us correctness:
 * passing VLC's pool-managed picture buffers directly to per-stripe zimg
 * graphs from worker threads is unreliable in ways we couldn't isolate
 * via instrumentation. The pure pattern - per-stripe graphs, persistent
 * thread pool, fresh pinned buffers - was verified working in isolation
 * (standalone reproducer + in-VLC-process self-test on fresh buffers).
 * Only the direct VLC-picture path failed, so we go through scratch.
 *
 * THREADING
 *
 * N persistent worker threads spawned at Open(), one per stripe. Each
 * worker owns its zimg_filter_graph (built for that stripe's src_h/dst_h
 * dimensions) and its tmp buffer. Per-frame dispatch via per-worker
 * sem_t "go" + a shared sem_t "done" the main thread waits on N times.
 * N is determined by up_threads_decide() in threading.h.
 *
 * Stripe-boundary caveat: each stripe's graph resamples independently
 * with zimg's default boundary handling. For natural video content the
 * effect is invisible; on stylized content with pixel-sharp horizontal
 * lines a user can fall back to --autoupscale-threads=1.
 *
 * Supported chromas: I420, YV12, I422, I444. NV12/NV21/RGB go to swscale.
 *
 * Built only when HAVE_ZIMG is defined.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"
#include "upscale_logic.h"
#include "threading.h"
#include "zimg_helpers.h"
#include "scaler_zimg_chroma.h"

#include <zimg.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGN_DOWN_2(x) UP_ALIGN_DOWN_2(x)

/* Bundled plane pointers + pitch + line counts for one side (src or dst).
 * Used both by the per-worker view (where lines_* are unused but cheap to
 * carry) and the priv-level scratch buffer geometry. */
typedef struct
{
    uint8_t *y, *u, *v;
    int      pitch_y, pitch_c;
    int      lines_y, lines_c;
} plane_set_t;

/* Per-worker state. */
typedef struct
{
    /* _Alignas(64) on the first member promotes the whole struct's
     * alignment to 64 and forces sizeof to a 64-byte multiple, so an
     * aligned-allocated array keeps each worker on its own cache line(s).
     * Per dispatch the main thread writes per-worker fields (sem_t.go
     * atomics; in zerocopy-dst mode also w->dst.{y,u,v,pitch_*}) and the
     * worker writes w->result; without padding, two adjacent workers'
     * writes invalidate each other's lines on every frame. Same fix as
     * usm_worker_t in usm_pool.c. C11 disallows _Alignas on a typedef
     * name, hence on the first member. */
    _Alignas(64) pthread_t  thread;
    sem_t              go;
    sem_t             *done;          /* shared with parent */
    bool               thread_started; /* true iff pthread_create succeeded;
                                        * pthread_t is opaque, so a "== 0"
                                        * test on `thread` is not portable —
                                        * mirror usm_worker_t and gate join/
                                        * signal on this flag instead. */
    /* should_exit (main->worker) and result (worker->main) are plain ints
     * shared across threads. They are race-free ONLY because of the
     * sem_post(go)/sem_wait(done) handshake around every dispatch: those
     * POSIX-semaphore ops are full memory barriers, giving the writes
     * happens-before the reads. Keep that handshake on every path or these
     * must become _Atomic. */
    int                should_exit;

    /* Persistent: one graph + one tmp buffer per worker. */
    zimg_filter_graph *graph;
    void              *tmp;
    size_t             tmp_size;

    /* Stripe geometry (constant after Open). */
    int                src_y_start;   /* in luma rows */
    int                dst_y_start;
    int                worker_id;
    unsigned           sub_h;

    /* Pointers into the parent's scratch buffers (set at Open).
     * .lines_y / .lines_c are unused at the worker level. */
    plane_set_t        src;
    plane_set_t        dst;

    int                result;        /* 0 OK, -1 fail */
} stripe_worker_t;

typedef struct
{
    int               n_threads;
    stripe_worker_t  *workers;
    sem_t             done;
    bool              done_inited;   /* sem_destroy guard: true iff sem_init succeeded */

    int               yv12_swap_uv;
    unsigned          sub_w, sub_h;

    /* Scratch buffers + geometry. Sized + allocated on first Filter() call
     * (lazy init), pinned for the plugin lifetime after that. Open() is
     * kept cheap so VLC's chain solver can probe us without paying for 30
     * worker thread spawns and 6 MB of scratch per probe. */
    plane_set_t       src;
    plane_set_t       dst;
    int               src_w, src_h, dst_w, dst_h;

    /* Lazy-init state. lazy_init_done is set by zimg_lazy_init() after
     * the worker pool, scratch, and per-stripe graphs are constructed
     * successfully. lazy_init_failed sticks once the first attempt has
     * failed so we don't retry-allocate every frame. The algo and
     * log_obj are saved at Open() time so lazy_init can build graphs
     * and emit its diagnostic message without needing the ctx. */
    bool              lazy_init_done;
    bool              lazy_init_failed;
    int               algo_saved;
    void             *log_obj_saved;

    /* Destination zero-copy mode. When true, workers write directly into
     * VLC's destination picture; the dy/du/dv pointers and dst pitch
     * fields above are NOT allocated and are instead overwritten per
     * frame from the picture passed to zimg_process(). Skips the final
     * memcpy back from scratch -> VLC dst (~125 us/frame at 1080p).
     * Opt-in via the autoupscale-zerocopy-dst module option; default off. */
    bool              dst_zerocopy;
} zimg_priv_t;

/* ---------- chroma + algo mappings ---------- */

/*
 * Thin wrapper around up_chroma_to_zimg() in scaler_zimg_chroma.h.
 * The pure logic lives in the header so the same code is exercised by
 * the production path AND by tests/fuzz_scaler_chroma.c without
 * pulling in VLC headers. vlc_fourcc_t is a uint32_t, so the cast is
 * a no-op at runtime.
 */
static int ChromaToZimg(vlc_fourcc_t c,
                        unsigned *sub_w, unsigned *sub_h, int *yv12_swap)
{
    return up_chroma_to_zimg((uint32_t)c, sub_w, sub_h, yv12_swap);
}

static zimg_resample_filter_e AlgoToZimg(int algo)
{
    switch (algo)
    {
        case UP_ALGO_FAST_BILINEAR: return ZIMG_RESIZE_BILINEAR;
        case UP_ALGO_BICUBIC:       return ZIMG_RESIZE_BICUBIC;
        case UP_ALGO_LANCZOS:       return ZIMG_RESIZE_LANCZOS;
        case UP_ALGO_SPLINE36:
        default:                    return ZIMG_RESIZE_SPLINE36;
    }
}

/* ---------- worker thread ---------- */

/*
 * Internal: fill one plane of a zimg image buffer (writable). The const
 * variant aliases the same layout (plane.data is `void *` in both, so
 * casting through here is safe). data + offset_rows*stride is the row-0
 * pointer for this stripe; the mask covers the full graph height
 * (BUFFER_MAX = no wraparound). CCN 1.
 */
static inline void set_buf_plane(zimg_image_buffer *b, int idx,
                                 const void *data, int stride,
                                 int offset_rows)
{
    b->plane[idx].data   = (uint8_t *)(uintptr_t)data
                         + (size_t)offset_rows * (size_t)stride;
    b->plane[idx].stride = stride;
    b->plane[idx].mask   = ZIMG_BUFFER_MAX;
}

static void *worker_main(void *arg)
{
    stripe_worker_t *w = (stripe_worker_t *)arg;
    for (;;)
    {
        sem_wait(&w->go);
        if (w->should_exit) break;

        /* Zero-init buffer descriptors. Upstream zimg's API contract
         * (see doc/example/api_example_c.c) requires plane[3] to be
         * zero when alpha is absent — `data == NULL` is the signal.
         * The braced initializer satisfies C99 §6.7.8/21 which zeros
         * all unmentioned members. Compared to memset+assignment, gcc
         * -O2 emits ~50% fewer stores here because it elides zero-
         * stores to fields immediately overwritten below (plane[0..2]).
         * Same idiom upstream uses, also seen in mpv and ffmpeg.
         *
         * The diagnostic suppression is needed because -Wextra warns
         * on the unmentioned `plane` member even though C99 explicitly
         * defines the behavior. Limited to these two lines. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
        zimg_image_buffer_const sb = { ZIMG_API_VERSION };
        zimg_image_buffer       db = { ZIMG_API_VERSION };
#pragma GCC diagnostic pop

        const int src_off_y = w->src_y_start;
        const int dst_off_y = w->dst_y_start;
        const int src_off_c = w->src_y_start >> w->sub_h;
        const int dst_off_c = w->dst_y_start >> w->sub_h;

        const uint8_t *src_planes[3] = { w->src.y, w->src.u, w->src.v };
        uint8_t       *dst_planes[3] = { w->dst.y, w->dst.u, w->dst.v };
        const int src_strides[3] = { w->src.pitch_y, w->src.pitch_c, w->src.pitch_c };
        const int dst_strides[3] = { w->dst.pitch_y, w->dst.pitch_c, w->dst.pitch_c };
        const int src_offs[3]    = { src_off_y, src_off_c, src_off_c };
        const int dst_offs[3]    = { dst_off_y, dst_off_c, dst_off_c };
        for (int p = 0; p < 3; p++) {
            set_buf_plane((zimg_image_buffer *)&sb, p,
                          src_planes[p], src_strides[p], src_offs[p]);
            set_buf_plane(&db, p,
                          dst_planes[p], dst_strides[p], dst_offs[p]);
        }

        zimg_error_code_e rc = zimg_filter_graph_process(
            w->graph, &sb, &db, w->tmp, NULL, NULL, NULL, NULL);
        w->result = (rc == 0) ? 0 : -1;

        sem_post(w->done);
    }
    return NULL;
}

/* ---------- per-stripe graph builder ---------- */

static zimg_filter_graph *build_stripe_graph(
    int src_w, int src_stripe_h,
    int dst_w, int dst_stripe_h,
    unsigned sub_w, unsigned sub_h,
    zimg_resample_filter_e filt)
{
    zimg_image_format src_fmt, dst_fmt;
    zimg_image_format_default(&src_fmt, ZIMG_API_VERSION);
    zimg_image_format_default(&dst_fmt, ZIMG_API_VERSION);

    src_fmt.width        = src_w;
    src_fmt.height       = src_stripe_h;
    src_fmt.pixel_type   = ZIMG_PIXEL_BYTE;
    src_fmt.subsample_w  = sub_w;
    src_fmt.subsample_h  = sub_h;
    src_fmt.color_family = ZIMG_COLOR_YUV;

    dst_fmt = src_fmt;
    dst_fmt.width  = dst_w;
    dst_fmt.height = dst_stripe_h;

    zimg_graph_builder_params params;
    zimg_graph_builder_params_default(&params, ZIMG_API_VERSION);
    params.resample_filter    = filt;
    params.resample_filter_uv = filt;
    params.cpu_type           = ZIMG_CPU_AUTO;

    return zimg_filter_graph_build(&src_fmt, &dst_fmt, &params);
}

/* ---------- backend ops ---------- */

static int zimg_supports(vlc_fourcc_t chroma, int algo)
{
    (void)algo;
    unsigned sw, sh;
    int swap;
    return ChromaToZimg(chroma, &sw, &sh, &swap);
}

static void zimg_close(scaler_ctx_t *ctx);

/* Compute stripe bounds: see up_compute_stripe_bounds in zimg_helpers.h. */

/*
 * Allocate the persistent scratch buffers (one per plane). The src side
 * (sy/su/sv) is always allocated - workers cannot read directly from
 * VLC's pool-managed source buffers (segfaults observed). The dst side
 * (dy/du/dv) is allocated only when dst_zerocopy is OFF; with zero-copy
 * enabled, workers write straight into VLC's destination picture and
 * the dst scratch is unused.
 *
 * Returns 0 on success, -1 on any allocation failure. Partial state is
 * freed by zimg_close via priv->src.* / priv->dst.*. CCN 4.
 */
/*
 * Compute `lines * pitch` as size_t with overflow check. Returns 0 if
 * either input is non-positive or if the multiplication would wrap.
 * In practice both come from up_plane_lines / up_plane_pitch which
 * already cap inputs, but we double-check at the alloc seam so a
 * malformed dimension reaching this code can never produce an
 * undersized buffer that downstream copies write past.
 */
static inline size_t plane_alloc_bytes(int lines, int pitch)
{
    if (lines <= 0 || pitch <= 0) return 0;
    size_t l = (size_t)lines, pp = (size_t)pitch;
    if (l > SIZE_MAX / pp) return 0;
    return l * pp;
}

/* Allocate one plane_set_t's three plane buffers. Returns 0 on success,
 * -1 if any size computation overflows or any aligned_alloc fails. CCN 4. */
static int alloc_one_plane_set(plane_set_t *ps)
{
    const size_t y_bytes = plane_alloc_bytes(ps->lines_y, ps->pitch_y);
    const size_t c_bytes = plane_alloc_bytes(ps->lines_c, ps->pitch_c);
    if (y_bytes == 0 || c_bytes == 0) return -1;

    ps->y = aligned_alloc(UP_PITCH_ALIGN, y_bytes);
    ps->u = aligned_alloc(UP_PITCH_ALIGN, c_bytes);
    ps->v = aligned_alloc(UP_PITCH_ALIGN, c_bytes);
    if (!ps->y || !ps->u || !ps->v) return -1;
    return 0;
}

static int alloc_scratch_buffers(zimg_priv_t *p)
{
    if (alloc_one_plane_set(&p->src) != 0) return -1;
    if (p->dst_zerocopy)
        return 0;  /* dst.{y,u,v} stay NULL, set per-frame from VLC dst */
    return alloc_one_plane_set(&p->dst);
}

/*
 * Fill the priv struct's geometry/pitch fields from the scaler context
 * and chroma subsampling. Pure assignment; no allocation. CCN 1.
 */
static void init_priv_geometry(zimg_priv_t *p, const scaler_ctx_t *ctx,
                               unsigned sub_w, unsigned sub_h, int swap)
{
    p->yv12_swap_uv = swap;
    p->sub_w        = sub_w;
    p->sub_h        = sub_h;
    p->src_w = ctx->src_w; p->src_h = ctx->src_h;
    p->dst_w = ctx->dst_w; p->dst_h = ctx->dst_h;

    p->src.pitch_y = up_plane_pitch(ctx->src_w, 0);
    p->src.pitch_c = up_plane_pitch(ctx->src_w, sub_w);
    p->dst.pitch_y = up_plane_pitch(ctx->dst_w, 0);
    p->dst.pitch_c = up_plane_pitch(ctx->dst_w, sub_w);
    p->src.lines_y = up_plane_lines(ctx->src_h, 0);
    p->src.lines_c = up_plane_lines(ctx->src_h, sub_h);
    p->dst.lines_y = up_plane_lines(ctx->dst_h, 0);
    p->dst.lines_c = up_plane_lines(ctx->dst_h, sub_h);
}

/*
 * Set up one stripe worker: stash geometry, build the per-stripe filter
 * graph, allocate its tmp buffer, init its semaphore, and spawn the
 * thread. Returns 0 on success, -1 on any failure (caller handles
 * cleanup of partial state via the worker's graph/tmp fields). CCN 5.
 */
static int init_stripe_worker(stripe_worker_t *w, zimg_priv_t *p,
                              const scaler_ctx_t *ctx, int worker_id,
                              int src_y_start, int src_y_end,
                              int dst_y_start, int dst_y_end,
                              unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt)
{
    w->src_y_start  = src_y_start;
    w->dst_y_start  = dst_y_start;
    /* worker_main shifts row offsets by sub_h (`>> w->sub_h`); a value >=
     * the int width would be UB. Valid YUV chroma gives 0 or 1, but clamp
     * defensively so a malformed sub_h can never reach the shift. */
    w->sub_h        = (sub_h < 8u) ? sub_h : 0u;
    w->done         = &p->done;
    w->worker_id    = worker_id;
    w->src = p->src;
    w->dst = p->dst;

    w->graph = build_stripe_graph(
        ctx->src_w, src_y_end - src_y_start,
        ctx->dst_w, dst_y_end - dst_y_start,
        sub_w, sub_h, filt);
    if (!w->graph) return -1;

    if (zimg_filter_graph_get_tmp_size(w->graph, &w->tmp_size) != 0)
        return -1;
    if (w->tmp_size > 0) {
        w->tmp = aligned_alloc(UP_PITCH_ALIGN, (w->tmp_size + UP_PITCH_ALIGN - 1) & ~(size_t)(UP_PITCH_ALIGN - 1));
        if (!w->tmp) return -1;
    }
    if (sem_init(&w->go, 0, 0) != 0) return -1;
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        sem_destroy(&w->go);
        return -1;
    }
    w->thread_started = true;
    return 0;
}

/*
 * Per-worker teardown primitive: release the graph, tmp buffer, semaphore,
 * and joined thread held by ONE worker slot. Idempotent — safe on a
 * fully-constructed worker, a partially-constructed one, or a zeroed slot.
 *
 * `had_thread` lets the caller indicate whether the thread field is a
 * valid pthread handle that needs joining (true) or a stub left over from
 * a failed pthread_create (false). Callers pass w->thread_started; we
 * can't infer it from `w->thread` alone because pthread_t is opaque.
 *
 * After return, all dynamic resources owned by `w` are released; the
 * struct itself is NOT zeroed (caller decides whether to reuse the slot).
 */
static void release_worker_resources(stripe_worker_t *w, bool had_thread)
{
    if (had_thread) {
        w->should_exit = true;
        sem_post(&w->go);
        pthread_join(w->thread, NULL);
        sem_destroy(&w->go);
    }
    if (w->graph) { zimg_filter_graph_free(w->graph); w->graph = NULL; }
    free(w->tmp); w->tmp = NULL;
}

/*
 * Tear down all `n` constructed workers and zero their slots so they can
 * be re-initialized cleanly on retry. Used after a partial construction.
 * CCN 2 (was CCN 5).
 */
static void teardown_constructed_workers(zimg_priv_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        stripe_worker_t *w = &p->workers[i];
        release_worker_resources(w, w->thread_started);
        memset(w, 0, sizeof *w);
    }
}

/*
 * Try to spawn one stripe worker at index `i` covering its share of
 * [0, dst_h) when partitioned into `n` stripes. Returns 0 on success,
 * -1 on degenerate stripe geometry or worker init failure. On failure
 * any partially-allocated graph/tmp inside `w` is released.
 */
static int try_spawn_one_worker(zimg_priv_t *p, const scaler_ctx_t *ctx,
                                int i, int n,
                                unsigned sub_w, unsigned sub_h,
                                zimg_resample_filter_e filt)
{
    stripe_worker_t *w = &p->workers[i];

    int sys, sye, dys, dye;
    if (!up_compute_stripe_bounds(i, n, ctx->src_h, ctx->dst_h,
                                  &sys, &sye, &dys, &dye))
        return -1;

    if (init_stripe_worker(w, p, ctx, i, sys, sye, dys, dye,
                           sub_w, sub_h, filt) != 0) {
        /* init_stripe_worker may have allocated graph or tmp before
         * failing. Release those without touching thread/sem (no thread
         * was started since pthread_create is the last step). */
        release_worker_resources(w, false);
        return -1;
    }
    return 0;
}

/*
 * Try to construct exactly `n` stripe workers covering [0, dst_h).
 * Returns the number successfully built (≤ n). On partial build
 * (a stripe degenerated or worker init failed), the partial set is
 * left in p->workers — caller may use it directly, OR tear it down
 * and retry with a smaller n.
 */
static int try_construct_workers(zimg_priv_t *p, const scaler_ctx_t *ctx,
                                 int n, unsigned sub_w, unsigned sub_h,
                                 zimg_resample_filter_e filt)
{
    int constructed = 0;
    for (int i = 0; i < n; i++) {
        if (try_spawn_one_worker(p, ctx, i, n, sub_w, sub_h, filt) != 0)
            break;
        constructed++;
    }
    return constructed;
}

/*
 * Construct N stripe workers covering the full destination height.
 *
 * If try_construct_workers returns fewer than requested (a stripe
 * degenerated), we tear down the partial set and retry with the
 * smaller count. This is essential for correctness: without retry,
 * the LAST stripe of a partial construction would not extend to
 * dst_h (its end is computed against the original `n`, not the
 * realized count), leaving an unwritten band of rows at the bottom
 * of every output frame — visible as a black or garbage strip.
 *
 * The retry converges in at most a few iterations since each retry
 * uses a strictly smaller `n`. Returns 0 on success (with at least
 * one worker covering the full height), -1 on total failure.
 */
static void construct_workers(zimg_priv_t *p, const scaler_ctx_t *ctx,
                              int n_threads, unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt,
                              int *out_constructed)
{
    int n = n_threads;
    int constructed = 0;

    while (n > 0) {
        constructed = try_construct_workers(p, ctx, n, sub_w, sub_h, filt);
        /* Done either way: fully covered (constructed == n), or total
         * failure (constructed == 0) — nothing left to retry with. */
        if (constructed == n || constructed == 0) break;

        /* Partial build: the realized count is smaller than n, so
         * the last stripe doesn't extend to dst_h. Tear down and
         * retry with n = constructed so the new last stripe correctly
         * closes the partition at dst_h. */
        teardown_constructed_workers(p, constructed);
        n = constructed;
        constructed = 0;
    }

    *out_constructed = constructed;
    p->n_threads = constructed;
}

/* Diagnostic log emitted once at Open(). CCN 2. */
static void log_zimg_open(vlc_object_t *log_obj, const zimg_priv_t *p)
{
    if (!log_obj) return;
    size_t src_mb = ((size_t)p->src.lines_y * p->src.pitch_y
                   + 2 * (size_t)p->src.lines_c * p->src.pitch_c) >> 20;
    size_t dst_mb = p->dst_zerocopy ? 0
        : (((size_t)p->dst.lines_y * p->dst.pitch_y
          + 2 * (size_t)p->dst.lines_c * p->dst.pitch_c) >> 20);
    msg_Info(log_obj,
             "zimg: %d worker thread%s, %dx%d -> %dx%d, "
             "scratch %zu MB (%s)",
             p->n_threads, p->n_threads == 1 ? "" : "s",
             p->src_w, p->src_h, p->dst_w, p->dst_h,
             src_mb + dst_mb,
             p->dst_zerocopy ? "copy-in/zero-copy-out"
                             : "copy-in/copy-out");
}

/*
 * Internal: bail out of zimg_open after the priv struct exists. Stashes
 * the partial priv on the context so zimg_close() can free what was
 * already allocated (scratch buffers, workers array, semaphore). CCN 1.
 */
/*
 * Lazy initialization of the worker pool, scratch buffers, and per-stripe
 * filter graphs. Called on the first zimg_process() invocation rather
 * than from zimg_open().
 *
 * Why defer this? VLC's filter-chain solver instantiates filters
 * speculatively while searching for a working chain. With hardware
 * decode + a non-trivial filter chain (e.g. postproc + autoupscale),
 * the solver may construct and tear down our filter 3-4 times before
 * settling on a working configuration. Each of those Open()/Close()
 * round trips spawning 30 worker threads + allocating 6 MB of scratch
 * is wasteful. Doing it lazily means VLC pays nothing for probes that
 * never produce a frame; only the first real Filter() call triggers
 * the expensive setup.
 *
 * Returns 0 on success, -1 on any allocation/thread-spawn failure.
 * On failure the priv is left in a partial state and lazy_init_failed
 * is set so subsequent Filter() calls return -1 immediately rather
 * than retry-allocating every frame.
 */
static int zimg_lazy_init(zimg_priv_t *p)
{
    if (alloc_scratch_buffers(p) != 0) return -1;

    /* aligned_alloc not calloc: stripe_worker_t carries _Alignas(64) so
     * each element sits on its own cache line; calloc returns malloc-
     * default (16) alignment which would defeat the layout. sizeof is
     * already a multiple of 64 thanks to _Alignas, satisfying
     * aligned_alloc's C11 size constraint. Manual memset replaces
     * calloc's zero-init. */
    {
        size_t total = (size_t)p->n_threads * sizeof(*p->workers);
        p->workers = aligned_alloc(64, total);
        if (!p->workers) return -1;
        memset(p->workers, 0, total);
    }

    if (sem_init(&p->done, 0, 0) != 0) return -1;
    p->done_inited = true;

    int constructed = 0;
    /* construct_workers needs ctx-shaped data: build a fake scaler_ctx_t
     * with just the fields it reads (src/dst geometry). The priv struct
     * has all of those already from init_priv_geometry. */
    scaler_ctx_t fake_ctx;
    memset(&fake_ctx, 0, sizeof fake_ctx);
    fake_ctx.src_w = p->src_w; fake_ctx.src_h = p->src_h;
    fake_ctx.dst_w = p->dst_w; fake_ctx.dst_h = p->dst_h;

    construct_workers(p, &fake_ctx, p->n_threads, p->sub_w, p->sub_h,
                      (zimg_resample_filter_e)p->algo_saved, &constructed);
    if (constructed == 0) return -1;

    log_zimg_open((vlc_object_t *)p->log_obj_saved, p);
    return 0;
}

/*
 * zimg_open: cheap setup only. Validates that we can handle the input
 * chroma, computes geometry and thread count, allocates the priv struct,
 * and returns. Does NOT spawn workers, allocate scratch, or build
 * per-stripe graphs - that all happens lazily on the first Filter()
 * call. See zimg_lazy_init() for rationale.
 */
static int zimg_open(scaler_ctx_t *ctx)
{
    unsigned sub_w, sub_h;
    int swap;
    if (!ChromaToZimg(ctx->chroma, &sub_w, &sub_h, &swap))
        return -1;

    int n_threads = up_threads_decide(ctx->threads_pref, up_detect_cores());

    /* Each stripe at least stripe_min_lines dst rows tall so kernel
     * context is meaningful. Helper resolves the 0-sentinel to the
     * compile-time default; see src/zimg_helpers.h. */
    int stripe_min_lines = up_zimg_stripe_min_lines(ctx->zimg.min_stripe_lines);
    int max_threads_by_size = ctx->dst_h / stripe_min_lines;
    if (max_threads_by_size < 1) max_threads_by_size = 1;
    if (n_threads > max_threads_by_size) n_threads = max_threads_by_size;

    zimg_priv_t *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->n_threads = n_threads;
    init_priv_geometry(p, ctx, sub_w, sub_h, swap);

    /* Save what zimg_lazy_init() needs that isn't already in priv. */
    p->algo_saved    = (int)AlgoToZimg(ctx->algo);
    p->log_obj_saved = ctx->log_obj;
    p->dst_zerocopy  = (ctx->zimg.zerocopy != 0);

    ctx->priv = p;
    return 0;
}

/*
 * Internal: copy 3 YUV planes between a VLC picture and our scratch
 * plane_set_t, in either direction. Plane 0 is luma at (w,h); planes
 * 1/2 are chroma at (w>>sub_w, h>>sub_h). The ps planes alternate role
 * (src or dst) depending on `dir`: when dir==0 ps is the destination
 * (copy IN to scratch); when dir==1 ps is the source (copy OUT to pic).
 */
static void copy_yuv_planes(const plane_set_t *ps, picture_t *pic, int swap,
                            int w, int h, unsigned sub_w, unsigned sub_h,
                            int dir)
{
    const int idx[3] = { 0, up_zimg_plane_idx(1, swap),
                            up_zimg_plane_idx(2, swap) };
    uint8_t   *ps_data[3]   = { ps->y, ps->u, ps->v };
    const int  ps_pitch[3]  = { ps->pitch_y, ps->pitch_c, ps->pitch_c };
    const int cw = (w + (1 << sub_w) - 1) >> sub_w;
    const int ch = (h + (1 << sub_h) - 1) >> sub_h;
    const int pw[3] = { w, cw, cw };
    const int ph[3] = { h, ch, ch };

    for (int k = 0; k < 3; k++) {
        uint8_t *pic_data  = pic->p[idx[k]].p_pixels;
        int      pic_pitch = pic->p[idx[k]].i_pitch;
        if (dir == 0)
            up_copy_plane(ps_data[k], ps_pitch[k],
                          pic_data, pic_pitch, pw[k], ph[k]);
        else
            up_copy_plane(pic_data, pic_pitch,
                          ps_data[k], ps_pitch[k], pw[k], ph[k]);
    }
}

/*
 * Phase 1: Copy IN. VLC's source picture (which we cannot read from
 * worker threads — pool-managed buffers segfault) is copied to our
 * scratch source buffers. Workers then read from the scratch.
 */
static void zimg_copy_in(zimg_priv_t *p, const picture_t *src)
{
    /* picture_t is read-only here, but copy_yuv_planes also serves the
     * write path; cast away const at the seam. */
    copy_yuv_planes(&p->src, (picture_t *)src, p->yv12_swap_uv,
                    p->src_w, p->src_h, p->sub_w, p->sub_h, 0);
}

/*
 * Phase 2 (only when zero-copy dst is enabled): point each worker at
 * VLC's destination picture for this frame. Workers read these fields
 * fresh on each dispatch (they're blocked on `go` until sem_post), so
 * no synchronization is needed. The per-stripe filter graphs do not
 * bake in dst stride — that goes into the per-call zimg_image_buffer
 * inside worker_main.
 */
static void zimg_zerocopy_point_workers(zimg_priv_t *p, const picture_t *dst)
{
    const int swap = p->yv12_swap_uv;
    int d_y = 0;
    int d_u = up_zimg_plane_idx(1, swap);
    int d_v = up_zimg_plane_idx(2, swap);
    for (int i = 0; i < p->n_threads; i++) {
        stripe_worker_t *w = &p->workers[i];
        w->dst.y       = dst->p[d_y].p_pixels;
        w->dst.u       = dst->p[d_u].p_pixels;
        w->dst.v       = dst->p[d_v].p_pixels;
        w->dst.pitch_y = dst->p[d_y].i_pitch;
        w->dst.pitch_c = dst->p[d_u].i_pitch;
    }
}

/*
 * Phase 3: Dispatch all workers and wait for completion.
 * Returns 0 if every worker succeeded, -1 otherwise.
 */
static int zimg_dispatch_and_wait(zimg_priv_t *p)
{
    for (int i = 0; i < p->n_threads; i++) {
        p->workers[i].result = 0;
        sem_post(&p->workers[i].go);
    }
    for (int i = 0; i < p->n_threads; i++)
        sem_wait(&p->done);
    for (int i = 0; i < p->n_threads; i++) {
        if (p->workers[i].result != 0) return -1;
    }
    return 0;
}

/*
 * Phase 4 (only when zero-copy dst is OFF): copy the scratch
 * destination buffers back to VLC's dst picture. Skipped entirely
 * with zero-copy, since workers already wrote into VLC's dst above.
 */
static void zimg_copy_out(zimg_priv_t *p, picture_t *dst)
{
    copy_yuv_planes(&p->dst, dst, p->yv12_swap_uv,
                    p->dst_w, p->dst_h, p->sub_w, p->sub_h, 1);
}

static int zimg_process(scaler_ctx_t *ctx,
                        const picture_t *src, picture_t *dst)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return -1;

    /* Lazy init: spawn workers, allocate scratch, build per-stripe
     * graphs. Done once on the first frame so VLC's chain solver can
     * probe us cheaply during chain setup. */
    if (!p->lazy_init_done) {
        if (p->lazy_init_failed) return -1;
        if (zimg_lazy_init(p) != 0) {
            p->lazy_init_failed = true;
            return -1;
        }
        p->lazy_init_done = true;
    }

    zimg_copy_in(p, src);

    if (p->dst_zerocopy)
        zimg_zerocopy_point_workers(p, dst);

    if (zimg_dispatch_and_wait(p) != 0)
        return -1;

    if (!p->dst_zerocopy)
        zimg_copy_out(p, dst);

    return 0;
}

/*
 * Signal every started worker to exit, in a separate pass before joining,
 * so all sem_posts go out before we block on the first join. Joining one
 * thread at a time would serialize the wakeups; this batches them.
 */
static void zimg_signal_all_workers_exit(zimg_priv_t *p)
{
    for (int i = 0; i < p->n_threads; i++) {
        stripe_worker_t *w = &p->workers[i];
        if (w->thread_started) {
            w->should_exit = 1;
            sem_post(&w->go);
        }
    }
}

static void zimg_close(scaler_ctx_t *ctx)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return;

    if (p->workers) {
        zimg_signal_all_workers_exit(p);
        for (int i = 0; i < p->n_threads; i++) {
            stripe_worker_t *w = &p->workers[i];
            release_worker_resources(w, w->thread_started);
        }
        free(p->workers);
    }
    if (p->done_inited) sem_destroy(&p->done);

    free(p->src.y); free(p->src.u); free(p->src.v);
    free(p->dst.y); free(p->dst.u); free(p->dst.v);
    free(p);
    ctx->priv = NULL;
}

const scaler_backend_t scaler_backend_zimg_impl = {
    .name     = "zimg",
    .id       = SCALER_BACKEND_ZIMG,
    .supports = zimg_supports,
    .open     = zimg_open,
    .process  = zimg_process,
    .close    = zimg_close,
};
