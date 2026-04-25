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

#include <zimg.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGN_DOWN_2(x) UP_ALIGN_DOWN_2(x)

/* Per-worker state. */
typedef struct
{
    pthread_t          thread;
    sem_t              go;
    sem_t             *done;          /* shared with parent */
    int                should_exit;

    /* Persistent: one graph + one tmp buffer per worker. */
    zimg_filter_graph *graph;
    void              *tmp;
    size_t             tmp_size;

    /* Stripe geometry (constant after Open). */
    int                src_y_start;   /* in luma rows */
    int                dst_y_start;
    int                worker_id;

    /* Pointers into the parent's scratch buffers (set at Open). */
    uint8_t           *sy, *su, *sv;  /* src luma/U/V */
    uint8_t           *dy, *du, *dv;  /* dst luma/U/V */
    int                src_pitch_y, src_pitch_c;
    int                dst_pitch_y, dst_pitch_c;
    unsigned           sub_h;

    int                result;        /* 0 OK, -1 fail */
} stripe_worker_t;

typedef struct
{
    int               n_threads;
    stripe_worker_t  *workers;
    sem_t             done;

    int               yv12_swap_uv;
    unsigned          sub_w, sub_h;

    /* Scratch buffers. Sized at Open(), pinned for the plugin lifetime. */
    uint8_t          *sy, *su, *sv;
    uint8_t          *dy, *du, *dv;
    int               src_w, src_h, dst_w, dst_h;
    int               src_pitch_y, src_pitch_c;
    int               dst_pitch_y, dst_pitch_c;
    int               src_lines_y,  src_lines_c;
    int               dst_lines_y,  dst_lines_c;
} zimg_priv_t;

/* ---------- chroma + algo mappings ---------- */

static int ChromaToZimg(vlc_fourcc_t c,
                        unsigned *sub_w, unsigned *sub_h, int *yv12_swap)
{
    *yv12_swap = 0;
    switch (c)
    {
        case VLC_CODEC_I420: *sub_w = 1; *sub_h = 1; return 1;
        case VLC_CODEC_YV12: *sub_w = 1; *sub_h = 1; *yv12_swap = 1; return 1;
        case VLC_CODEC_I422: *sub_w = 1; *sub_h = 0; return 1;
        case VLC_CODEC_I444: *sub_w = 0; *sub_h = 0; return 1;
        default:             return 0;
    }
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
 * Internal: fill one plane of a const (read) zimg image buffer.
 * data + offset_rows*stride is the row-0 pointer for this stripe; the
 * mask covers the full graph height (BUFFER_MAX = no wraparound). CCN 1.
 */
static inline void set_src_plane(zimg_image_buffer_const *b, int idx,
                                 const uint8_t *data, int stride,
                                 int offset_rows)
{
    b->plane[idx].data   = data + (size_t)offset_rows * (size_t)stride;
    b->plane[idx].stride = stride;
    b->plane[idx].mask   = ZIMG_BUFFER_MAX;
}

/* Same, for the writable destination buffer. CCN 1. */
static inline void set_dst_plane(zimg_image_buffer *b, int idx,
                                 uint8_t *data, int stride,
                                 int offset_rows)
{
    b->plane[idx].data   = data + (size_t)offset_rows * (size_t)stride;
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

        zimg_image_buffer_const sb;
        zimg_image_buffer       db;
        memset(&sb, 0, sizeof sb);
        memset(&db, 0, sizeof db);
        sb.version = ZIMG_API_VERSION;
        db.version = ZIMG_API_VERSION;

        const int src_off_y = w->src_y_start;
        const int dst_off_y = w->dst_y_start;
        const int src_off_c = w->src_y_start >> w->sub_h;
        const int dst_off_c = w->dst_y_start >> w->sub_h;

        set_src_plane(&sb, 0, w->sy, w->src_pitch_y, src_off_y);
        set_src_plane(&sb, 1, w->su, w->src_pitch_c, src_off_c);
        set_src_plane(&sb, 2, w->sv, w->src_pitch_c, src_off_c);
        set_dst_plane(&db, 0, w->dy, w->dst_pitch_y, dst_off_y);
        set_dst_plane(&db, 1, w->du, w->dst_pitch_c, dst_off_c);
        set_dst_plane(&db, 2, w->dv, w->dst_pitch_c, dst_off_c);

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

static long detect_cores(void)
{
    long c = sysconf(_SC_NPROCESSORS_ONLN);
    return c > 0 ? c : 1;
}

static void zimg_close(scaler_ctx_t *ctx);

/* Compute stripe bounds: see up_compute_stripe_bounds in zimg_helpers.h. */

/*
 * Allocate the persistent scratch buffers (one per plane, src + dst).
 * Returns 0 on success, -1 on any allocation failure (in which case the
 * partially-allocated state is freed by zimg_close via priv->sy etc).
 * CCN 4.
 */
static int alloc_scratch_buffers(zimg_priv_t *p)
{
    p->sy = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->src_lines_y * p->src_pitch_y);
    p->su = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->src_lines_c * p->src_pitch_c);
    p->sv = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->src_lines_c * p->src_pitch_c);
    p->dy = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->dst_lines_y * p->dst_pitch_y);
    p->du = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->dst_lines_c * p->dst_pitch_c);
    p->dv = aligned_alloc(UP_PITCH_ALIGN, (size_t)p->dst_lines_c * p->dst_pitch_c);
    if (!p->sy || !p->su || !p->sv || !p->dy || !p->du || !p->dv)
        return -1;
    return 0;
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

    p->src_pitch_y = up_plane_pitch(ctx->src_w, 0);
    p->src_pitch_c = up_plane_pitch(ctx->src_w, sub_w);
    p->dst_pitch_y = up_plane_pitch(ctx->dst_w, 0);
    p->dst_pitch_c = up_plane_pitch(ctx->dst_w, sub_w);
    p->src_lines_y = up_plane_lines(ctx->src_h, 0);
    p->src_lines_c = up_plane_lines(ctx->src_h, sub_h);
    p->dst_lines_y = up_plane_lines(ctx->dst_h, 0);
    p->dst_lines_c = up_plane_lines(ctx->dst_h, sub_h);
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
    w->sub_h        = sub_h;
    w->done         = &p->done;
    w->worker_id    = worker_id;
    w->sy = p->sy; w->su = p->su; w->sv = p->sv;
    w->dy = p->dy; w->du = p->du; w->dv = p->dv;
    w->src_pitch_y = p->src_pitch_y; w->src_pitch_c = p->src_pitch_c;
    w->dst_pitch_y = p->dst_pitch_y; w->dst_pitch_c = p->dst_pitch_c;

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
    return 0;
}

/*
 * Construct N stripe workers. May return fewer than n_threads if a
 * stripe degenerates or any worker fails to construct - those partial
 * graphs/tmp buffers are released here before returning. The number of
 * fully-constructed workers is written to *out_constructed. CCN 5.
 */
static void construct_workers(zimg_priv_t *p, const scaler_ctx_t *ctx,
                              int n_threads, unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt,
                              int *out_constructed)
{
    int constructed = 0;
    for (int i = 0; i < n_threads; i++) {
        stripe_worker_t *w = &p->workers[i];

        int sys, sye, dys, dye;
        if (!up_compute_stripe_bounds(i, n_threads, ctx->src_h, ctx->dst_h,
                                   &sys, &sye, &dys, &dye))
            break;

        if (init_stripe_worker(w, p, ctx, i, sys, sye, dys, dye,
                               sub_w, sub_h, filt) != 0) {
            if (w->graph) { zimg_filter_graph_free(w->graph); w->graph = NULL; }
            free(w->tmp); w->tmp = NULL;
            break;
        }
        constructed++;
    }
    *out_constructed = constructed;
    p->n_threads = constructed;
}

/* Diagnostic log emitted once at Open(). CCN 2. */
static void log_zimg_open(vlc_object_t *log_obj, const zimg_priv_t *p)
{
    if (!log_obj) return;
    size_t scratch_mb = (
          (size_t)p->src_lines_y * p->src_pitch_y
        + 2 * (size_t)p->src_lines_c * p->src_pitch_c
        + (size_t)p->dst_lines_y * p->dst_pitch_y
        + 2 * (size_t)p->dst_lines_c * p->dst_pitch_c) >> 20;
    msg_Info(log_obj,
             "zimg: %d worker thread%s, %dx%d -> %dx%d, "
             "scratch %zu MB (copy-in/copy-out)",
             p->n_threads, p->n_threads == 1 ? "" : "s",
             p->src_w, p->src_h, p->dst_w, p->dst_h, scratch_mb);
}

/*
 * Internal: bail out of zimg_open after the priv struct exists. Stashes
 * the partial priv on the context so zimg_close() can free what was
 * already allocated (scratch buffers, workers array, semaphore). CCN 1.
 */
static int zimg_open_fail(scaler_ctx_t *ctx, zimg_priv_t *p)
{
    ctx->priv = p;
    zimg_close(ctx);
    return -1;
}

static int zimg_open(scaler_ctx_t *ctx)
{
    unsigned sub_w, sub_h;
    int swap;
    if (!ChromaToZimg(ctx->chroma, &sub_w, &sub_h, &swap))
        return -1;

    int n_threads = up_threads_decide(ctx->threads_pref, detect_cores());

    /* Each stripe at least UP_STRIPE_MIN_DST_LINES dst rows tall so kernel
     * context is meaningful. */
    int max_threads_by_size = ctx->dst_h / UP_STRIPE_MIN_DST_LINES;
    if (max_threads_by_size < 1) max_threads_by_size = 1;
    if (n_threads > max_threads_by_size) n_threads = max_threads_by_size;

    zimg_priv_t *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->n_threads = n_threads;
    init_priv_geometry(p, ctx, sub_w, sub_h, swap);

    if (alloc_scratch_buffers(p) != 0) return zimg_open_fail(ctx, p);

    p->workers = calloc((size_t)n_threads, sizeof(*p->workers));
    if (!p->workers) return zimg_open_fail(ctx, p);

    if (sem_init(&p->done, 0, 0) != 0) return zimg_open_fail(ctx, p);

    int constructed = 0;
    construct_workers(p, ctx, n_threads, sub_w, sub_h,
                      AlgoToZimg(ctx->algo), &constructed);
    if (constructed == 0) return zimg_open_fail(ctx, p);

    log_zimg_open(ctx->log_obj, p);
    ctx->priv = p;
    return 0;
}

static int zimg_process(scaler_ctx_t *ctx,
                        const picture_t *src, picture_t *dst)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return -1;

    /* Copy IN: VLC's source picture -> our scratch source buffers. */
    {
        const int swap = p->yv12_swap_uv;
        int s_y = 0, s_u = up_zimg_plane_idx(1, swap), s_v = up_zimg_plane_idx(2, swap);
        up_copy_plane(p->sy, p->src_pitch_y,
                   src->p[s_y].p_pixels, src->p[s_y].i_pitch,
                   p->src_w, p->src_h);
        const int cw = (p->src_w + (1 << p->sub_w) - 1) >> p->sub_w;
        const int ch = (p->src_h + (1 << p->sub_h) - 1) >> p->sub_h;
        up_copy_plane(p->su, p->src_pitch_c,
                   src->p[s_u].p_pixels, src->p[s_u].i_pitch, cw, ch);
        up_copy_plane(p->sv, p->src_pitch_c,
                   src->p[s_v].p_pixels, src->p[s_v].i_pitch, cw, ch);
    }

    /* Dispatch all workers, wait for all. */
    for (int i = 0; i < p->n_threads; i++) {
        p->workers[i].result = 0;
        sem_post(&p->workers[i].go);
    }
    for (int i = 0; i < p->n_threads; i++)
        sem_wait(&p->done);

    for (int i = 0; i < p->n_threads; i++) {
        if (p->workers[i].result != 0) return -1;
    }

    /* Copy OUT: scratch destination buffers -> VLC's dst picture. */
    {
        const int swap = p->yv12_swap_uv;
        int d_y = 0, d_u = up_zimg_plane_idx(1, swap), d_v = up_zimg_plane_idx(2, swap);
        up_copy_plane(dst->p[d_y].p_pixels, dst->p[d_y].i_pitch,
                   p->dy, p->dst_pitch_y, p->dst_w, p->dst_h);
        const int cw = (p->dst_w + (1 << p->sub_w) - 1) >> p->sub_w;
        const int ch = (p->dst_h + (1 << p->sub_h) - 1) >> p->sub_h;
        up_copy_plane(dst->p[d_u].p_pixels, dst->p[d_u].i_pitch,
                   p->du, p->dst_pitch_c, cw, ch);
        up_copy_plane(dst->p[d_v].p_pixels, dst->p[d_v].i_pitch,
                   p->dv, p->dst_pitch_c, cw, ch);
    }

    return 0;
}

static void zimg_close(scaler_ctx_t *ctx)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return;

    if (p->workers) {
        for (int i = 0; i < p->n_threads; i++) {
            stripe_worker_t *w = &p->workers[i];
            if (w->thread) {
                w->should_exit = 1;
                sem_post(&w->go);
            }
        }
        for (int i = 0; i < p->n_threads; i++) {
            stripe_worker_t *w = &p->workers[i];
            if (w->thread) {
                pthread_join(w->thread, NULL);
                sem_destroy(&w->go);
            }
            if (w->graph) zimg_filter_graph_free(w->graph);
            free(w->tmp);
        }
        free(p->workers);
    }
    sem_destroy(&p->done);

    free(p->sy); free(p->su); free(p->sv);
    free(p->dy); free(p->du); free(p->dv);
    free(p);
    ctx->priv = NULL;
}

const scaler_backend_t scaler_backend_zimg_impl = {
    .name     = "zimg",
    .supports = zimg_supports,
    .open     = zimg_open,
    .process  = zimg_process,
    .close    = zimg_close,
};
