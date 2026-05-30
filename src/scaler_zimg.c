// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_zimg.c - zimg backend with slice-threaded resampling
 *****************************************************************************
 * Higher-quality alternative to swscale (Spline36 + tighter rounding) plus
 * slice threading: the output frame is split into N horizontal stripes and
 * processed in parallel by a persistent worker pool.
 *
 * COPY-IN / COPY-OUT (zero-copy on each side, independently)
 *
 * Each side of the resample can either go through a persistent, page-aligned
 * scratch buffer or touch VLC's picture directly:
 *
 *   - SOURCE. Default `zerocopy-src=1`: each worker's graph reads VLC's
 *     source picture directly (no copy, no scratch src). `zerocopy-src=0`:
 *     each worker copies its own source stripe into scratch first (PERF-1,
 *     parallel) and the graph reads the scratch.
 *   - DEST. Default `zerocopy-dst=1`: each worker's graph writes VLC's
 *     destination picture directly (no copy, no scratch dst). `zerocopy-dst=0`:
 *     the graph writes scratch and each worker copies its own dst stripe out
 *     to VLC's picture (PERF-5, parallel).
 *
 * So in the default config there is NO per-frame copy and no per-frame scratch
 * — worker graphs read and write VLC's pictures directly. Pointing graphs at
 * VLC's pool-managed buffers from worker threads was historically unreliable;
 * the dest zero-copy default proved the pattern works, source zero-copy is the
 * symmetric twin, and a per-frame pre-flight check (zimg_pic_ok) drops a frame
 * rather than read/write a malformed picture out of bounds. Either side can be
 * set back to copy via its option if a particular VLC build misbehaves. The
 * four src×dst copy/zero-copy combinations are held byte-identical by the test
 * harness (tests/test_scaler_zimg.c).
 *
 * THREADING
 *
 * N persistent worker threads spawned at Open(), one per stripe. Each
 * worker owns its zimg_filter_graph (built for that stripe's src_h/dst_h
 * dimensions) and its tmp buffer. Per-frame dispatch is O(1) syscalls on the
 * main thread (SCAL-2): the wake side bumps a shared "generation" under a
 * mutex and wakes all workers with ONE pthread_cond broadcast, and completion
 * is a counting barrier — workers decrement an atomic "pending", the last
 * posting a single "all_done" sem the main thread waits on once. (Was N
 * sem_post + N sem_wait per frame.)
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
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
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
     * Per dispatch the main thread writes per-worker fields (w->result reset;
     * in zerocopy-dst mode also w->dst.{y,u,v,pitch_*}) and the worker writes
     * w->result and w->seen_gen; without padding, two adjacent workers'
     * writes invalidate each other's lines on every frame. Same fix as
     * usm_worker_t in usm_pool.c. C11 disallows _Alignas on a typedef
     * name, hence on the first member. */
    alignas(64) pthread_t  thread;
    /* SCAL-2: the wake side is a single broadcast gate, not N per-worker
     * sems. The main thread bumps *generation once per dispatch under *go_lock
     * and broadcasts *go_cv; each worker sleeps until *generation advances past
     * its own seen_gen. The done side is a counting barrier: each worker
     * decrements *pending after its stripe; the one that drives it to zero
     * posts *all_done, which the main thread waits on exactly once. All four
     * shared pointers point into the parent zimg_priv_t. */
    pthread_mutex_t   *go_lock;
    pthread_cond_t    *go_cv;
    uint64_t          *generation;   /* shared, guarded by *go_lock */
    uint64_t           seen_gen;     /* worker-private: last dispatch handled */
    atomic_int        *pending;
    sem_t             *all_done;
    bool               thread_started; /* true iff pthread_create succeeded;
                                        * pthread_t is opaque, so a "== 0"
                                        * test on `thread` is not portable —
                                        * mirror usm_worker_t and gate join/
                                        * signal on this flag instead. */
    /* should_exit (main->worker): set by zimg_wake_all_for_exit() under
     * *go_lock together with a *generation bump + broadcast; the worker reads
     * it under the same lock in its wait predicate, so the access is fully
     * mutex-synchronized (no atomics needed). result is single-writer (worker)
     * / single-reader (main): each worker writes result, then does an acq_rel
     * fetch_sub on *pending; those RMWs form a release sequence, so the worker
     * that hits zero (acquire) observes every other worker's result write, and
     * its sem_post(all_done) -> main's sem_wait(all_done) publishes them. */
    int                should_exit;

    /* Persistent: one graph + one tmp buffer per worker. */
    zimg_filter_graph *graph;
    void              *tmp;
    size_t             tmp_size;

    /* Stripe geometry (constant after Open). */
    int                src_y_start;   /* in luma rows */
    int                src_y_end;     /* in luma rows (copy-in stripe) */
    int                dst_y_start;
    int                dst_y_end;     /* in luma rows (copy-out stripe) */
    int                worker_id;
    int                src_w;         /* luma width (copy-in stripe) */
    int                dst_w;         /* luma width (copy-out stripe) */
    unsigned           sub_w, sub_h;

    /* Per-worker I/O mode (constant after Open):
     *   copy_in  = !src_zerocopy: worker memcpys VLC src -> scratch src.
     *   copy_out = !dst_zerocopy: worker memcpys scratch dst -> VLC dst.
     * With zero-copy on the matching side, the zimg graph reads/writes the
     * VLC picture directly and the copy is skipped. */
    bool               copy_in;
    bool               copy_out;

    /* The buffers the zimg graph reads from / writes to. In a copy mode these
     * point at the pool scratch (set at Open); in a zero-copy mode they are
     * overwritten per frame with the VLC picture planes. */
    plane_set_t        src;
    plane_set_t        dst;

    /* VLC picture planes for the CURRENT frame, set per dispatch (while the
     * worker is blocked on `go`, so no synchronization needed):
     *   vlc_src — copy_in source  (PERF-1: each worker copies its own stripe)
     *   vlc_dst — copy_out target (PERF-5: each worker copies its own stripe)
     * Unused on the zero-copy side (the graph touches the VLC picture). */
    plane_set_t        vlc_src;
    plane_set_t        vlc_dst;

    int                result;        /* 0 OK, -1 fail */
} stripe_worker_t;

typedef struct
{
    int               n_threads;
    stripe_worker_t  *workers;
    /* SCAL-2 wake gate: one broadcast wakes all workers (was N sem_post). */
    pthread_mutex_t   go_lock;
    pthread_cond_t    go_cv;
    uint64_t          generation;    /* bumped per dispatch + on exit, under go_lock */
    bool              go_gate_inited; /* destroy guard: mutex+cond init'd */
    atomic_int        pending;       /* SCAL-2: live workers this dispatch */
    sem_t             all_done;      /* posted once when pending hits 0 */
    bool              all_done_inited; /* sem_destroy guard: true iff sem_init succeeded */

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
     * and emit its diagnostic message without needing the ctx.
     *
     * CON-2: lazy_init_done / lazy_init_failed are PLAIN bools, read+written
     * with no atomics or lock. This is sound only under the contract that a
     * single filter instance's zimg_process() (driven by VLC's Filter()) is
     * never entered concurrently — VLC calls a filter's pf_video_filter
     * serially per instance. If this scaler is ever shared across threads
     * within one instance, gate the first-frame init with pthread_once (or
     * make these _Atomic) to close the check-then-act window. */
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

    /* Source zero-copy: when true, worker graphs read the VLC source picture
     * directly (no copy-in, no scratch src). Symmetric to dst_zerocopy and,
     * like it, ON by default (autoupscale-zerocopy-src) — set the option to 0
     * to fall back to copy-in. See the COPY-IN/COPY-OUT note at the top. */
    bool              src_zerocopy;

    /* One-shot guard so the per-frame picture-geometry check (pre-flight)
     * warns once instead of every dropped frame. */
    bool              preflight_warned;
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

/*
 * PERF-1: copy this worker's source stripe from the VLC source picture into
 * the shared scratch source buffer. Each worker owns a disjoint luma row range
 * [src_y_start, src_y_end) (chroma shifted by sub_h), so there is no
 * cross-worker contention and no barrier — the copy folds into the same
 * dispatch as the resample instead of running as a serial main-thread pre-pass.
 * CCN 1.
 */
static void worker_copy_in_stripe(stripe_worker_t *w)
{
    const int rows = w->src_y_end - w->src_y_start;
    up_copy_plane(
        w->src.y     + (size_t)w->src_y_start * (size_t)w->src.pitch_y,
        w->src.pitch_y,
        w->vlc_src.y + (size_t)w->src_y_start * (size_t)w->vlc_src.pitch_y,
        w->vlc_src.pitch_y,
        w->src_w, rows);

    const int cs    = w->src_y_start >> w->sub_h;
    const int crows = (w->src_y_end >> w->sub_h) - cs;
    const int cw    = up_chroma_dim(w->src_w, (int)w->sub_w);
    up_copy_plane(
        w->src.u     + (size_t)cs * (size_t)w->src.pitch_c, w->src.pitch_c,
        w->vlc_src.u + (size_t)cs * (size_t)w->vlc_src.pitch_c, w->vlc_src.pitch_c,
        cw, crows);
    up_copy_plane(
        w->src.v     + (size_t)cs * (size_t)w->src.pitch_c, w->src.pitch_c,
        w->vlc_src.v + (size_t)cs * (size_t)w->vlc_src.pitch_c, w->vlc_src.pitch_c,
        cw, crows);
}

/*
 * PERF-5: mirror of worker_copy_in_stripe for the OUTPUT side. Copy this
 * worker's destination stripe from the scratch dst buffer out to the VLC
 * destination picture, when dst zero-copy is OFF. Disjoint dst rows per
 * worker -> no barrier; the copy-out parallelizes instead of running as a
 * serial main-thread post-pass. CCN 1.
 */
static void worker_copy_out_stripe(stripe_worker_t *w)
{
    const int rows = w->dst_y_end - w->dst_y_start;
    up_copy_plane(
        w->vlc_dst.y + (size_t)w->dst_y_start * (size_t)w->vlc_dst.pitch_y,
        w->vlc_dst.pitch_y,
        w->dst.y     + (size_t)w->dst_y_start * (size_t)w->dst.pitch_y,
        w->dst.pitch_y,
        w->dst_w, rows);

    const int cs    = w->dst_y_start >> w->sub_h;
    const int crows = (w->dst_y_end >> w->sub_h) - cs;
    const int cw    = up_chroma_dim(w->dst_w, (int)w->sub_w);
    up_copy_plane(
        w->vlc_dst.u + (size_t)cs * (size_t)w->vlc_dst.pitch_c, w->vlc_dst.pitch_c,
        w->dst.u     + (size_t)cs * (size_t)w->dst.pitch_c, w->dst.pitch_c,
        cw, crows);
    up_copy_plane(
        w->vlc_dst.v + (size_t)cs * (size_t)w->vlc_dst.pitch_c, w->vlc_dst.pitch_c,
        w->dst.v     + (size_t)cs * (size_t)w->dst.pitch_c, w->dst.pitch_c,
        cw, crows);
}

/* SCAL-2: block until the main thread bumps *generation (new dispatch) or
 * sets should_exit. Returns true to run a frame, false to exit the loop.
 * Runs under *go_lock; cond_wait handles spurious wakeups via the predicate. */
static bool worker_wait_for_go(stripe_worker_t *w)
{
    pthread_mutex_lock(w->go_lock);
    while (*w->generation == w->seen_gen && !w->should_exit)
        pthread_cond_wait(w->go_cv, w->go_lock);
    bool run = !w->should_exit;
    w->seen_gen = *w->generation;
    pthread_mutex_unlock(w->go_lock);
    return run;
}

static void *worker_main(void *arg)
{
    stripe_worker_t *w = (stripe_worker_t *)arg;
    for (;;)
    {
        if (!worker_wait_for_go(w)) break;

        if (w->copy_in) worker_copy_in_stripe(w);  /* PERF-1: parallel copy-in */

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

        if (w->copy_out) worker_copy_out_stripe(w);  /* PERF-5: parallel copy-out */

        /* SCAL-2: last worker to finish posts all_done exactly once. acq_rel
         * so the result writes above join the release sequence on *pending. */
        if (atomic_fetch_sub_explicit(w->pending, 1, memory_order_acq_rel) == 1)
            sem_post(w->all_done);
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
    /* Each side's scratch is allocated only when that side COPIES. With
     * zero-copy on a side, the graph reads/writes the VLC picture directly and
     * the scratch stays NULL (set per-frame from the VLC picture instead). */
    if (!p->src_zerocopy && alloc_one_plane_set(&p->src) != 0) return -1;
    if (!p->dst_zerocopy && alloc_one_plane_set(&p->dst) != 0) return -1;
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
                              int worker_id,
                              int src_y_start, int src_y_end,
                              int dst_y_start, int dst_y_end,
                              unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt)
{
    w->src_y_start  = src_y_start;
    w->src_y_end    = src_y_end;
    w->dst_y_start  = dst_y_start;
    w->dst_y_end    = dst_y_end;
    w->src_w        = p->src_w;
    w->dst_w        = p->dst_w;
    /* worker_main shifts row offsets by sub (`>> w->sub_h`); a value >= the
     * int width would be UB. Valid YUV chroma gives 0 or 1, but clamp both
     * exponents defensively so a malformed value can never reach the shift
     * (UB-2, UB-3). */
    w->sub_h        = (sub_h < 8u) ? sub_h : 0u;
    w->sub_w        = (sub_w < 8u) ? sub_w : 0u;
    /* I/O mode: copy on the side that is NOT zero-copy. */
    w->copy_in      = !p->src_zerocopy;
    w->copy_out     = !p->dst_zerocopy;
    w->go_lock      = &p->go_lock;
    w->go_cv        = &p->go_cv;
    w->generation   = &p->generation;
    w->seen_gen     = p->generation;   /* don't run a frame before the first dispatch */
    w->pending      = &p->pending;
    w->all_done     = &p->all_done;
    w->worker_id    = worker_id;
    w->src = p->src;
    w->dst = p->dst;

    w->graph = build_stripe_graph(
        p->src_w, src_y_end - src_y_start,
        p->dst_w, dst_y_end - dst_y_start,
        sub_w, sub_h, filt);
    if (!w->graph) return -1;

    if (zimg_filter_graph_get_tmp_size(w->graph, &w->tmp_size) != 0)
        return -1;
    if (w->tmp_size > 0) {
        w->tmp = aligned_alloc(UP_PITCH_ALIGN, (w->tmp_size + UP_PITCH_ALIGN - 1) & ~(size_t)(UP_PITCH_ALIGN - 1));
        if (!w->tmp) return -1;
    }
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
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
 * A started worker MUST already have been told to exit via
 * zimg_wake_all_for_exit() before this joins it — that signal happens once,
 * under go_lock, for every worker at once.
 *
 * After return, all dynamic resources owned by `w` are released; the
 * struct itself is NOT zeroed (caller decides whether to reuse the slot).
 */
static void release_worker_resources(stripe_worker_t *w, bool had_thread)
{
    if (had_thread)
        pthread_join(w->thread, NULL);
    if (w->graph) { zimg_filter_graph_free(w->graph); w->graph = NULL; }
    free(w->tmp); w->tmp = NULL;
}

/*
 * SCAL-2: tell every started worker to finish its loop. Sets should_exit on
 * all of them and bumps the generation under go_lock, then a single broadcast
 * wakes them — one wake for the whole pool, not one sem_post per worker. The
 * workers read should_exit under the same lock, so the writes never race their
 * reads. Always pair with a later release_worker_resources() that joins.
 */
static void zimg_wake_all_for_exit(zimg_priv_t *p)
{
    /* If the gate never initialized, no thread was ever spawned (construct
     * runs after gate init), so there is nothing to wake. */
    if (!p->go_gate_inited) return;
    pthread_mutex_lock(&p->go_lock);
    for (int i = 0; i < p->n_threads; i++)
        if (p->workers[i].thread_started)
            p->workers[i].should_exit = 1;
    p->generation++;
    pthread_cond_broadcast(&p->go_cv);
    pthread_mutex_unlock(&p->go_lock);
}

/*
 * Tear down all `n` constructed workers and zero their slots so they can
 * be re-initialized cleanly on retry. Used after a partial construction.
 * CCN 2 (was CCN 5).
 */
static void teardown_constructed_workers(zimg_priv_t *p, int n)
{
    /* One broadcast wakes every started worker (the broadcast reaches all
     * waiters; non-started slots are skipped), THEN join — same
     * signal-then-reap order as zimg_close. */
    zimg_wake_all_for_exit(p);
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
static int try_spawn_one_worker(zimg_priv_t *p,
                                int i, int n,
                                unsigned sub_w, unsigned sub_h,
                                zimg_resample_filter_e filt)
{
    stripe_worker_t *w = &p->workers[i];

    int sys, sye, dys, dye;
    if (!up_compute_stripe_bounds(i, n, p->src_h, p->dst_h,
                                  &sys, &sye, &dys, &dye))
        return -1;

    if (init_stripe_worker(w, p, i, sys, sye, dys, dye,
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
static int try_construct_workers(zimg_priv_t *p,
                                 int n, unsigned sub_w, unsigned sub_h,
                                 zimg_resample_filter_e filt)
{
    int constructed = 0;
    for (int i = 0; i < n; i++) {
        if (try_spawn_one_worker(p, i, n, sub_w, sub_h, filt) != 0)
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
static void construct_workers(zimg_priv_t *p,
                              int n_threads, unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt,
                              int *out_constructed)
{
    int n = n_threads;
    int constructed = 0;

    while (n > 0) {
        constructed = try_construct_workers(p, n, sub_w, sub_h, filt);
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
    /* Only scratch that is actually allocated (the copy side) counts. */
    size_t src_mb = p->src_zerocopy ? 0
        : (((size_t)p->src.lines_y * p->src.pitch_y
          + 2 * (size_t)p->src.lines_c * p->src.pitch_c) >> 20);
    size_t dst_mb = p->dst_zerocopy ? 0
        : (((size_t)p->dst.lines_y * p->dst.pitch_y
          + 2 * (size_t)p->dst.lines_c * p->dst.pitch_c) >> 20);
    msg_Info(log_obj,
             "zimg: %d worker thread%s, %dx%d -> %dx%d, scratch %zu MB "
             "(src %s, dst %s)",
             p->n_threads, p->n_threads == 1 ? "" : "s",
             p->src_w, p->src_h, p->dst_w, p->dst_h,
             src_mb + dst_mb,
             p->src_zerocopy ? "zero-copy" : "copy",
             p->dst_zerocopy ? "zero-copy" : "copy");
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

    atomic_init(&p->pending, 0);
    if (sem_init(&p->all_done, 0, 0) != 0) return -1;
    p->all_done_inited = true;

    /* SCAL-2 wake gate. Must be live before construct_workers spawns threads,
     * since each worker blocks on go_cv immediately (seen_gen == generation). */
    if (pthread_mutex_init(&p->go_lock, NULL) != 0) return -1;
    if (pthread_cond_init(&p->go_cv, NULL) != 0) {
        pthread_mutex_destroy(&p->go_lock);
        return -1;
    }
    p->generation = 0;
    p->go_gate_inited = true;

    int constructed = 0;
    /* ARCH-1: the worker-construction chain reads only src/dst geometry, all
     * of which lives in the priv struct (init_priv_geometry) — no fake ctx. */
    construct_workers(p, p->n_threads, p->sub_w, p->sub_h,
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
    /* REL-3: fail gracefully if the runtime libzimg is a different ABI major
     * than the headers we built against. zimg keeps source/ABI compat within
     * a major version (a higher minor only adds features), so only a major
     * mismatch is fatal. */
    unsigned z_major = 0, z_minor = 0;
    zimg_get_api_version(&z_major, &z_minor);
    if (z_major != ZIMG_API_VERSION_MAJOR) {
        if (ctx->log_obj)
            msg_Err((vlc_object_t *)ctx->log_obj,
                    "AutoUpscale: libzimg API v%u.%u incompatible with "
                    "built-against v%u.%u (major mismatch)",
                    z_major, z_minor,
                    (unsigned)ZIMG_API_VERSION_MAJOR,
                    (unsigned)ZIMG_API_VERSION_MINOR);
        return -1;
    }

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
    p->src_zerocopy  = (ctx->zimg.src_zerocopy != 0);

    ctx->priv = p;
    return 0;
}

/*
 * Per-frame: copy one VLC picture's plane pointers + pitches into the
 * `plane_set_t` member of every worker selected by `member_off` (an offsetof
 * into stripe_worker_t — one of src / dst / vlc_src / vlc_dst). Drives all
 * four per-frame pointer-sets through one loop (DUP-7/PERF-6):
 *   src      ← VLC src  when src zero-copy (graph reads VLC src)
 *   vlc_src  ← VLC src  when copy-in       (worker copies VLC src -> scratch)
 *   dst      ← VLC dst  when dst zero-copy  (graph writes VLC dst)
 *   vlc_dst  ← VLC dst  when copy-out       (worker copies scratch -> VLC dst)
 * Workers are blocked on `go` while this runs, so the writes need no
 * synchronization. YV12 U/V are swapped via the plane-index map. CCN 2.
 */
static void point_workers_planes(zimg_priv_t *p, const picture_t *pic,
                                  size_t member_off)
{
    const int swap = p->yv12_swap_uv;
    const int iy = 0;
    const int iu = up_zimg_plane_idx(1, swap);
    const int iv = up_zimg_plane_idx(2, swap);
    for (int i = 0; i < p->n_threads; i++) {
        plane_set_t *ps =
            (plane_set_t *)((char *)&p->workers[i] + member_off);
        ps->y       = pic->p[iy].p_pixels;
        ps->u       = pic->p[iu].p_pixels;
        ps->v       = pic->p[iv].p_pixels;
        ps->pitch_y = pic->p[iy].i_pitch;
        ps->pitch_c = pic->p[iu].i_pitch;
    }
}

/*
 * Dispatch all workers and wait for completion.
 * Returns 0 if every worker succeeded, -1 otherwise.
 */
static int zimg_dispatch_and_wait(zimg_priv_t *p)
{
    /* SCAL-2 wake side: arm the done-barrier and reset results, then bump the
     * generation once and wake every worker with a single broadcast (was N
     * sem_post). All of this under go_lock, so a worker that wakes sees pending
     * already armed and its result already reset. */
    pthread_mutex_lock(&p->go_lock);
    atomic_store_explicit(&p->pending, p->n_threads, memory_order_relaxed);
    for (int i = 0; i < p->n_threads; i++)
        p->workers[i].result = 0;
    p->generation++;
    pthread_cond_broadcast(&p->go_cv);
    pthread_mutex_unlock(&p->go_lock);

    sem_wait(&p->all_done);   /* done side: one wait (counting barrier) */
    for (int i = 0; i < p->n_threads; i++) {
        if (p->workers[i].result != 0) return -1;
    }
    return 0;
}

/*
 * Pre-flight: validate that a VLC picture we are about to read from / write to
 * is usable for luma width `w` — 3 planes present, plane pointers non-NULL,
 * and each pitch at least the visible width. Catches a malformed picture
 * (the zerocopy-dst "may not work everywhere" case) BEFORE the workers do an
 * out-of-bounds read/write. CCN 6.
 */
static bool zimg_pic_ok(const zimg_priv_t *p, const picture_t *pic, int w)
{
    if (pic->i_planes < 3) return false;
    const int swap = p->yv12_swap_uv;
    const int iy = 0;
    const int iu = up_zimg_plane_idx(1, swap);
    const int iv = up_zimg_plane_idx(2, swap);
    if (!pic->p[iy].p_pixels || !pic->p[iu].p_pixels || !pic->p[iv].p_pixels)
        return false;
    const int cw = up_chroma_dim(w, (int)p->sub_w);
    return pic->p[iy].i_pitch >= w
        && pic->p[iu].i_pitch >= cw
        && pic->p[iv].i_pitch >= cw;
}

/* offsetof selectors for the per-frame plane-pointer targets. */
#define WORKER_SRC_OFF      offsetof(stripe_worker_t, src)
#define WORKER_DST_OFF      offsetof(stripe_worker_t, dst)
#define WORKER_VLC_SRC_OFF  offsetof(stripe_worker_t, vlc_src)
#define WORKER_VLC_DST_OFF  offsetof(stripe_worker_t, vlc_dst)

/*
 * Lazy init on first frame: spawn workers, allocate scratch, build per-stripe
 * graphs — done once so VLC's chain solver can probe us cheaply during chain
 * setup. Returns 0 if ready (already or just initialized), -1 on sticky
 * failure. Extracted to keep zimg_process at CCN <= 10. CCN 5.
 */
static int zimg_ensure_lazy_init(zimg_priv_t *p)
{
    if (p->lazy_init_done) return 0;
    if (p->lazy_init_failed) return -1;
    if (zimg_lazy_init(p) != 0) {
        p->lazy_init_failed = true;
        /* OBS-2: the expensive setup ran at first frame, after the cheap
         * Open() succeeded; say so once, else every frame drops silently. */
        if (p->log_obj_saved)
            msg_Err((vlc_object_t *)p->log_obj_saved,
                    "zimg: worker/scratch init failed (%dx%d -> %dx%d); "
                    "AutoUpscale will drop frames",
                    p->src_w, p->src_h, p->dst_w, p->dst_h);
        return -1;
    }
    p->lazy_init_done = true;
    return 0;
}

static int zimg_process(scaler_ctx_t *ctx,
                        const picture_t *src, picture_t *dst)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return -1;
    if (zimg_ensure_lazy_init(p) != 0) return -1;

    /* Pre-flight guard: a malformed src/dst picture (null plane or pitch <
     * width) would make the workers read/write out of bounds — drop the frame
     * (warn once) instead. */
    if (!zimg_pic_ok(p, src, p->src_w) || !zimg_pic_ok(p, dst, p->dst_w)) {
        if (!p->preflight_warned) {
            p->preflight_warned = true;
            if (p->log_obj_saved)
                msg_Warn((vlc_object_t *)p->log_obj_saved,
                         "zimg: source/destination picture geometry unusable "
                         "(null plane or pitch < width); dropping frame(s)");
        }
        return -1;
    }

    /* Per-frame plane pointers. On each side the graph touches the VLC
     * picture directly (zero-copy) or the workers copy via scratch. */
    point_workers_planes(p, src,
        p->src_zerocopy ? WORKER_SRC_OFF : WORKER_VLC_SRC_OFF);
    point_workers_planes(p, dst,
        p->dst_zerocopy ? WORKER_DST_OFF : WORKER_VLC_DST_OFF);

    return zimg_dispatch_and_wait(p);
}

static void zimg_close(scaler_ctx_t *ctx)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return;

    if (p->workers) {
        zimg_wake_all_for_exit(p);   /* one broadcast, then join */
        for (int i = 0; i < p->n_threads; i++) {
            stripe_worker_t *w = &p->workers[i];
            release_worker_resources(w, w->thread_started);
        }
        free(p->workers);
    }
    if (p->go_gate_inited) {
        pthread_cond_destroy(&p->go_cv);
        pthread_mutex_destroy(&p->go_lock);
    }
    if (p->all_done_inited) sem_destroy(&p->all_done);

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
