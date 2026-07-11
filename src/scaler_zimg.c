// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_zimg.c - zimg backend with grid-threaded resampling
 *****************************************************************************
 * Higher-quality alternative to swscale (Spline36 + tighter rounding) plus a
 * persistent row×column worker grid. Normal frames use horizontal stripes;
 * wide/short frames may add column cells.
 *
 * COPY-IN / COPY-OUT (zero-copy on each side, independently)
 *
 * Each side can go through persistent, 64-byte-aligned scratch or touch VLC's
 * picture directly:
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
 * In the default aligned row-only config there is no plane-I/O copy or scratch:
 * worker graphs read and write VLC pictures directly. Column grids are the
 * exception: each cell copies private destination-tile scratch out. Pointing graphs at
 * VLC's pool-managed buffers from worker threads was historically unreliable;
 * the dest zero-copy default proved the pattern works, source zero-copy is the
 * symmetric twin, and a shared per-frame picture view rejects malformed
 * geometry before dispatch. First-frame direct-I/O misalignment selects
 * aligned scratch; later alignment drift drops that frame. Either side can be
 * set back to copy via its option if a particular VLC build misbehaves. The
 * copy/zero-copy combinations are byte-identical when the worker grid stays
 * the same. Source copy-in disables column tiling, so topology changes may
 * retain bounded graph-seam deltas (tests/test_scaler_zimg.c).
 *
 * THREADING
 *
 * Up to N preferred persistent workers spawn lazily on the first valid frame,
 * one per grid cell; geometry may yield fewer cells. The frame
 * is split into N_ROWS horizontal stripes; for frames too short for stripes
 * alone to use the worker preference (very wide / short), the grid also tiles N_COLS
 * columns (SCAL-3). A column tile reads the FULL-width source with zimg's
 * active_region cropping its column window (so zimg reads cross-boundary
 * source context, though independent graphs can retain bounded phase deltas)
 * and writes a per-worker tile-sized dst scratch that is
 * copied into the destination sub-rectangle. With N_COLS == 1 this is the
 * plain row-stripe path, byte-for-byte. Each worker owns its zimg_filter_graph
 * and tmp buffer. Per-frame dispatch is O(1) syscalls on the
 * main thread (SCAL-2): the wake side bumps a shared "generation" under a
 * mutex and wakes all workers with ONE pthread_cond broadcast, and completion
 * is a counting barrier — workers decrement an atomic "pending", the last
 * posting a single "all_done" sem the main thread waits on once. (Was N
 * sem_post + N sem_wait per frame.)
 * up_threads_decide() sets the affinity-capped budget; up_decide_tile_grid()
 * sets the effective grid size.
 *
 * Seam caveat: independent row/column graphs can retain bounded phase deltas.
 * A user sensitive to them can set --autoupscale-threads=1.
 *
 * Supported chromas: I420, YV12, I422, I444. NV12/NV21/RGB go to swscale.
 *
 * Built only when HAVE_ZIMG is defined.
 *****************************************************************************/

/* SCAL-4: pthread_setaffinity_np / CPU_ALLOC macros need _GNU_SOURCE before any
 * include. Defined unconditionally (harmless off-glibc, where the affinity
 * helper compiles to a no-op). */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"
#include "picture_view.h"
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
#if defined(__linux__)
# include <sched.h>
#endif

#define ALIGN_DOWN_2(x) UP_ALIGN_DOWN_2(x)

/* SCAL-3: a column tile narrower than this isn't worth its own zimg graph. */
#define ZIMG_COL_MIN_WIDTH 64
#define ZIMG_BUFFER_ALIGN 32

_Static_assert(UP_TILE_THREADS_MAX == UP_THREADS_MAX,
               "tile-grid and worker caps must match");

/*
 * SCAL-4: best-effort pin one worker thread to a single CPU core. Opt-in
 * (--autoupscale-pin-threads), isolates the non-portable pthread_setaffinity_np
 * here. Failure is ignored: pinning is an optimization, never a correctness
 * requirement, and can fail benignly (CPU offline, cgroup cpuset, container
 * limits). No-op where the CPU-affinity capability is absent.
 *
 * PORT-2: guard on the same UP_HAVE_CPU_AFFINITY capability macro that
 * threading.h derives (CPU_ALLOC family present AND !UP_NO_CPU_AFFINITY), not
 * a bare __linux__ — so a Linux libc lacking the CPU_ALLOC macros, or a build
 * that forces affinity off, degrades to a no-op instead of failing to compile. */
static void pin_worker_to_cpu(pthread_t thread, int cpu)
{
#if UP_HAVE_CPU_AFFINITY
    size_t set_size = CPU_ALLOC_SIZE((size_t)cpu + 1);
    cpu_set_t *set = CPU_ALLOC((size_t)cpu + 1);
    if (set == NULL) return;
    CPU_ZERO_S(set_size, set);
    CPU_SET_S((size_t)cpu, set_size, set);
    (void)pthread_setaffinity_np(thread, set_size, set);
    CPU_FREE(set);
#else
    (void)thread; (void)cpu;
#endif
}

enum { PLANE_Y, PLANE_U, PLANE_V, PLANE_COUNT };

typedef struct
{
    uint8_t *data[PLANE_COUNT];
    int      pitch[PLANE_COUNT];
} plane_view_t;

typedef struct
{
    int pitch[PLANE_COUNT];
    int lines[PLANE_COUNT];
} plane_layout_t;

typedef struct
{
    uint8_t       *data[PLANE_COUNT];
    plane_layout_t layout;
} plane_buffer_t;

typedef struct
{
    int      src_y_start;   /* in luma rows */
    int      src_y_end;     /* in luma rows (copy-in stripe) */
    int      dst_y_start;
    int      dst_y_end;     /* in luma rows (copy-out stripe) */
    int      dst_x_start;   /* luma column start (dst), copy-out placement */
    int      src_w;         /* luma TILE width (src) */
    int      dst_w;         /* luma TILE width (dst) */
    unsigned sub_w, sub_h;
} worker_cell_geom_t;

/* Per-worker state. */
typedef struct
{
    /* _Alignas(64) on the first member promotes the whole struct's
     * alignment to 64 and forces sizeof to a 64-byte multiple, so an
     * aligned-allocated array keeps each worker on its own cache line(s).
     * Per dispatch the main thread writes per-worker fields (w->result reset;
     * in zerocopy-dst mode also w->dst.{data,pitch}) and the worker writes
     * w->result and w->seen_gen; without padding, two adjacent workers'
     * writes invalidate each other's lines on every frame. Same fix as
     * usm_worker_t in usm_pool.c. C11 disallows _Alignas on a typedef
     * name, hence on the first member. */
    alignas(64) pthread_t  thread;
    /* Dispatch gate, owned by the priv and shared with the USM pool's
     * design via up_pool_gate_t (DUP-1) — see threading.h for the full
     * wake/done protocol and memory-ordering rationale. */
    up_pool_gate_t    *gate;
    uint64_t           seen_gen;     /* worker-private: last dispatch handled */
    bool               thread_started; /* true iff pthread_create succeeded;
                                        * pthread_t is opaque, so a "== 0"
                                        * test on `thread` is not portable —
                                        * mirror usm_worker_t and gate join/
                                        * signal on this flag instead. */
    /* should_exit (main->worker): set by zimg_wake_all_for_exit() under
     * the gate lock before a broadcast; the worker reads it under the same
     * lock and finishes any unseen generation before exiting. result is
     * single-writer (worker) / single-reader (main), published by the
     * gate's done barrier (see threading.h). */
    bool               should_exit;

    /* Persistent: one graph + one tmp buffer per worker. */
    zimg_filter_graph *graph;
    void              *tmp;
    size_t             tmp_size;

    /* Cell geometry (constant after lazy initialization). A worker owns a
     * row-stripe; with column tiling (SCAL-3) it also owns a column tile
     * [dst_x_start .. + dst_w). With n_cols==1 the column spans the full
     * width. */
    worker_cell_geom_t cell;
    int                worker_id;

    /* SCAL-3: true when this cell is a column tile. Then the graph reads the
     * full-width source (active_region crops columns, with halo) and writes a
     * per-worker tile-sized dst scratch (this struct owns w->tile_dst),
     * which worker_copy_out_tile() places into the VLC dst sub-rectangle. */
    bool               col_tiled;

    /* Per-worker I/O mode (constant after lazy initialization):
     *   copy_in  = !src_zerocopy: worker memcpys VLC src -> scratch src.
     *   copy_out = !dst_zerocopy: worker memcpys scratch dst -> VLC dst.
     * With zero-copy on the matching side, the zimg graph reads/writes the
     * VLC picture directly and the copy is skipped. */
    bool               copy_in;
    bool               copy_out;

    /* The buffers the zimg graph reads from / writes to. In a copy mode these
     * point at pool scratch (set during lazy init); in zero-copy mode they
     * are overwritten per frame with the VLC picture planes. */
    plane_view_t       src;
    plane_view_t       dst;
    plane_buffer_t     tile_dst;

    /* VLC picture planes for the CURRENT frame, set per dispatch (while the
     * worker is blocked on `go`, so no synchronization needed):
     *   vlc_src — copy_in source  (PERF-1: each worker copies its own stripe)
     *   vlc_dst — copy_out target (PERF-5: each worker copies its own stripe)
     * Unused on the zero-copy side (the graph touches the VLC picture). */
    plane_view_t       vlc_src;
    plane_view_t       vlc_dst;

    int                result;        /* 0 OK, -1 fail */
    zimg_error_code_e  err_code;      /* OBS-1: libzimg code when result != 0 */
} stripe_worker_t;

typedef struct
{
    stripe_worker_t  *workers;
    /* SCAL-2 wake gate: one broadcast wakes all workers (was N sem_post). */
    /* Shared wake gate + counting done-barrier (DUP-1, threading.h).
     * calloc zeroing marks it not-yet-initialized for destroy. */
    up_pool_gate_t    gate;
    bool              pool_broken;

    int               yv12_swap_uv;
    unsigned          sub_w, sub_h;

    /* SCAL-3/PAT-1: worker grid + zero-copy modes, resolved atomically by
     * the pure up_zimg_resolve_io_plan (zimg_helpers.h) and stored verbatim.
     * n_cols > 1 (column tiling) engages only when row stripes cannot use
     * the worker preference; then col_tiled forces source-direct read +
     * per-tile dst scratch. Otherwise n_cols == 1 (row-stripe path).
     * src/dst_zerocopy: graphs read/write the VLC picture directly; OFF
     * (via option or first-frame alignment) falls back to copy via scratch.
     * See the COPY-IN/COPY-OUT note at the top. */
    up_zimg_io_plan_t plan;
    int               worker_budget;  /* up_threads_decide result at open;
                                       * first-frame plan re-resolve input */

    /* REL-6: consecutive frames rejected for alignment drift; a full
     * streak escalates to FATAL. `warned` latches the one-shot log. */
    struct {
        int  streak;
        bool warned;
    } drift;

    /* SCAL-4/5: pin workers only to exact IDs in the allowed CPU set. */
    bool              pin_cpus;
    up_cpu_topology_t cpu_topology;

    /* Scratch buffers + geometry. Layouts are sized in Open(); buffers are
     * allocated on the first valid frame and retained for the plugin lifetime.
     * Open() stays cheap so speculative chain probes avoid full worker and
     * scratch setup. */
    plane_buffer_t    src;
    plane_buffer_t    dst;
    int               src_w, src_h, dst_w, dst_h;

    /* Lazy-init state. `done` is set by zimg_lazy_init() after the worker
     * pool, scratch, and per-cell graphs are constructed successfully.
     * `failed` sticks once the first attempt has failed so we don't
     * retry-allocate every frame. The algo and log_obj are saved at
     * Open() time so lazy_init can build graphs and emit its diagnostic
     * message without needing the ctx.
     *
     * CON-2: done / failed are PLAIN bools, read+written with no atomics
     * or lock. This is sound only under the contract that a single filter
     * instance's zimg_process() (driven by VLC's Filter()) is never
     * entered concurrently — VLC calls a filter's pf_video_filter
     * serially per instance. If this scaler is ever shared across threads
     * within one instance, gate the first-frame init with pthread_once (or
     * make these _Atomic) to close the check-then-act window. */
    struct {
        bool  done;
        bool  failed;
        int   algo;
        void *log_obj;
    } lazy;

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

/* Internal: fill one plane of a writable zimg image buffer. */
static inline void set_buf_plane(zimg_image_buffer *b, int idx,
                                 void *data, int stride,
                                 int offset_rows)
{
    b->plane[idx].data   = (uint8_t *)data
                         + (size_t)offset_rows * (size_t)stride;
    b->plane[idx].stride = stride;
    b->plane[idx].mask   = ZIMG_BUFFER_MAX;
}

/* zimg's const and writable descriptors are distinct tagged types. */
static inline void set_const_buf_plane(zimg_image_buffer_const *b, int idx,
                                       const void *data, int stride,
                                       int offset_rows)
{
    b->plane[idx].data   = (const uint8_t *)data
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
 */
static void worker_copy_in_stripe(const stripe_worker_t *w)
{
    const int rows = w->cell.src_y_end - w->cell.src_y_start;
    up_copy_plane(
        w->src.data[PLANE_Y]
            + (size_t)w->cell.src_y_start * (size_t)w->src.pitch[PLANE_Y],
        w->src.pitch[PLANE_Y],
        w->vlc_src.data[PLANE_Y]
            + (size_t)w->cell.src_y_start * (size_t)w->vlc_src.pitch[PLANE_Y],
        w->vlc_src.pitch[PLANE_Y],
        w->cell.src_w, rows);

    const int cs    = w->cell.src_y_start >> w->cell.sub_h;
    const int crows = (w->cell.src_y_end >> w->cell.sub_h) - cs;
    const int cw    = up_chroma_dim(w->cell.src_w, (int)w->cell.sub_w);
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        up_copy_plane(
            w->src.data[p] + (size_t)cs * (size_t)w->src.pitch[p],
            w->src.pitch[p],
            w->vlc_src.data[p] + (size_t)cs * (size_t)w->vlc_src.pitch[p],
            w->vlc_src.pitch[p], cw, crows);
    }
}

/*
 * PERF-5: mirror of worker_copy_in_stripe for the OUTPUT side. Copy this
 * worker's destination stripe from the scratch dst buffer out to the VLC
 * destination picture, when dst zero-copy is OFF. Disjoint dst rows per
 * worker -> no barrier; the copy-out parallelizes instead of running as a
 * serial main-thread post-pass.
 */
static void worker_copy_out_stripe(const stripe_worker_t *w)
{
    const int rows = w->cell.dst_y_end - w->cell.dst_y_start;
    up_copy_plane(
        w->vlc_dst.data[PLANE_Y]
            + (size_t)w->cell.dst_y_start * (size_t)w->vlc_dst.pitch[PLANE_Y],
        w->vlc_dst.pitch[PLANE_Y],
        w->dst.data[PLANE_Y]
            + (size_t)w->cell.dst_y_start * (size_t)w->dst.pitch[PLANE_Y],
        w->dst.pitch[PLANE_Y],
        w->cell.dst_w, rows);

    const int cs    = w->cell.dst_y_start >> w->cell.sub_h;
    const int crows = (w->cell.dst_y_end >> w->cell.sub_h) - cs;
    const int cw    = up_chroma_dim(w->cell.dst_w, (int)w->cell.sub_w);
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        up_copy_plane(
            w->vlc_dst.data[p] + (size_t)cs * (size_t)w->vlc_dst.pitch[p],
            w->vlc_dst.pitch[p],
            w->dst.data[p] + (size_t)cs * (size_t)w->dst.pitch[p],
            w->dst.pitch[p], cw, crows);
    }
}

/*
 * SCAL-3: place a column tile's output. The graph wrote the tile-sized scratch
 * w->dst (origin 0,0; tile pitch); copy it into the VLC dst sub-rectangle at
 * [dst_x_start, dst_y_start). Disjoint cells -> no barrier.
 */
static void worker_copy_out_tile(const stripe_worker_t *w)
{
    const int rows = w->cell.dst_y_end - w->cell.dst_y_start;
    const int cx   = w->cell.dst_x_start;
    up_copy_plane(
        w->vlc_dst.data[PLANE_Y]
            + (size_t)w->cell.dst_y_start * (size_t)w->vlc_dst.pitch[PLANE_Y] + cx,
        w->vlc_dst.pitch[PLANE_Y],
        w->dst.data[PLANE_Y], w->dst.pitch[PLANE_Y],
        w->cell.dst_w, rows);

    const int cs    = w->cell.dst_y_start >> w->cell.sub_h;
    const int crows = (w->cell.dst_y_end >> w->cell.sub_h) - cs;
    const int cw    = up_chroma_dim(w->cell.dst_w, (int)w->cell.sub_w);
    const int cxc   = w->cell.dst_x_start >> w->cell.sub_w;
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        up_copy_plane(
            w->vlc_dst.data[p]
                + (size_t)cs * (size_t)w->vlc_dst.pitch[p] + cxc,
            w->vlc_dst.pitch[p],
            w->dst.data[p], w->dst.pitch[p], cw, crows);
    }
}

/* Place this worker's resampled output: a column tile copies its private dst
 * scratch into the VLC dst sub-rect; a plain stripe copies out when dst is not
 * zero-copy (else the graph already wrote VLC's picture). */
static void worker_emit_output(const stripe_worker_t *w)
{
    if (w->col_tiled)     worker_copy_out_tile(w);    /* SCAL-3 */
    else if (w->copy_out) worker_copy_out_stripe(w);  /* PERF-5 */
}

static void *worker_main(void *arg)
{
    stripe_worker_t *w = (stripe_worker_t *)arg;
    for (;;)
    {
        if (!up_pool_gate_wait_for_go(w->gate, &w->seen_gen,
                                      &w->should_exit))
            break;

        if (w->copy_in) worker_copy_in_stripe(w);  /* PERF-1: parallel copy-in */

        /* Zero-init buffer descriptors. Upstream zimg's API contract
         * (see doc/example/api_example_c.c) requires plane[3] to be
         * zero when alpha is absent — `data == NULL` is the signal.
         * The braced initializer satisfies C99 §6.7.8/21 which zeros
         * all unmentioned members and lets the compiler elide stores to
         * fields immediately overwritten below.
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

        const int src_off_y = w->cell.src_y_start;
        const int src_off_c = w->cell.src_y_start >> w->cell.sub_h;
        /* A column tile writes its own tile scratch starting at row 0; a plain
         * stripe writes into the shared/VLC full-frame dst at its row offset. */
        const int dst_off_y = w->col_tiled ? 0 : w->cell.dst_y_start;
        const int dst_off_c = w->col_tiled ? 0 : (w->cell.dst_y_start >> w->cell.sub_h);

        const int src_offs[3]    = { src_off_y, src_off_c, src_off_c };
        const int dst_offs[3]    = { dst_off_y, dst_off_c, dst_off_c };
        for (int p = 0; p < 3; p++) {
            set_const_buf_plane(&sb, p,
                                w->src.data[p], w->src.pitch[p], src_offs[p]);
            set_buf_plane(&db, p,
                          w->dst.data[p], w->dst.pitch[p], dst_offs[p]);
        }

        zimg_error_code_e rc = zimg_filter_graph_process(
            w->graph, &sb, &db, w->tmp, NULL, NULL, NULL, NULL);
        w->result = (rc == 0) ? 0 : -1;
        w->err_code = rc;               /* OBS-1: keep the code for the log */

        worker_emit_output(w);

        up_pool_gate_worker_done(w->gate);
    }
    return NULL;
}

/* ---------- per-cell graph builder ---------- */

/*
 * Build one cell's graph. `src_full_w` is the FULL source width of the buffer
 * the graph will be handed; [act_left, act_left+act_width) is the source COLUMN
 * window this cell resamples (SCAL-3). When the window spans the full width the
 * active_region is left at its default (no crop) so the result is byte-identical
 * to the row-stripe-only path. `dst_w` is the cell's TILE dst width.
 */
/* One grid cell's ranges on both axes: rows partition heights, cols
 * partition widths (both produced by up_compute_stripe_bounds). */
typedef struct {
    up_stripe_bounds_t rows;
    up_stripe_bounds_t cols;
} cell_bounds_t;

static zimg_filter_graph *build_stripe_graph(
    int src_full_w, const cell_bounds_t *c,
    unsigned sub_w, unsigned sub_h,
    zimg_resample_filter_e filt)
{
    const int src_stripe_h = c->rows.src_end - c->rows.src_start;
    const int dst_stripe_h = c->rows.dst_end - c->rows.dst_start;
    const int act_left     = c->cols.src_start;
    const int act_width    = c->cols.src_end - c->cols.src_start;
    const int dst_w        = c->cols.dst_end - c->cols.dst_start;
    zimg_image_format src_fmt, dst_fmt;
    zimg_image_format_default(&src_fmt, ZIMG_API_VERSION);
    zimg_image_format_default(&dst_fmt, ZIMG_API_VERSION);

    src_fmt.width        = src_full_w;
    src_fmt.height       = src_stripe_h;
    src_fmt.pixel_type   = ZIMG_PIXEL_BYTE;
    src_fmt.subsample_w  = sub_w;
    src_fmt.subsample_h  = sub_h;
    src_fmt.color_family = ZIMG_COLOR_YUV;

    /* Column crop: zimg reads the active sub-window WITH halo from the full-
     * width buffer (safe — buffer width matches src_fmt.width). Even act_left/
     * width keep chroma exact. Skipped when the window is the whole width, so
     * the non-tiled path keeps the default active_region byte-for-byte. */
    if (act_left > 0 || act_width < src_full_w) {
        src_fmt.active_region.left  = (double)act_left;
        src_fmt.active_region.width = (double)act_width;
    }

    /* dst is a full tile image — keep its default (full) active_region; do NOT
     * inherit the source column window. */
    dst_fmt.pixel_type   = ZIMG_PIXEL_BYTE;
    dst_fmt.subsample_w  = sub_w;
    dst_fmt.subsample_h  = sub_h;
    dst_fmt.color_family = ZIMG_COLOR_YUV;
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
 * Allocate persistent scratch buffers (one per plane) only for a side using
 * copy I/O. Column cells own private destination-tile scratch instead of the
 * shared destination buffer.
 *
 * Returns 0 on success, -1 on any allocation failure. Partial state is
 * freed by zimg_close via priv->src.* / priv->dst.*.
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

static plane_view_t plane_buffer_view(const plane_buffer_t *buffer)
{
    plane_view_t view = {0};
    for (int p = 0; p < PLANE_COUNT; p++) {
        view.data[p] = buffer->data[p];
        view.pitch[p] = buffer->layout.pitch[p];
    }
    return view;
}

static void free_plane_buffer(plane_buffer_t *buffer)
{
    for (int p = 0; p < PLANE_COUNT; p++) {
        free(buffer->data[p]);
        buffer->data[p] = NULL;
    }
}

static void init_plane_layout(plane_layout_t *layout, int width, int height,
                              unsigned sub_w, unsigned sub_h)
{
    layout->pitch[PLANE_Y] = up_plane_pitch(width, 0);
    layout->lines[PLANE_Y] = up_plane_lines(height, 0);
    for (int p = PLANE_U; p < PLANE_COUNT; p++) {
        layout->pitch[p] = up_plane_pitch(width, sub_w);
        layout->lines[p] = up_plane_lines(height, sub_h);
    }
}

static size_t plane_buffer_bytes(const plane_buffer_t *buffer)
{
    size_t bytes = 0;
    for (int p = 0; p < PLANE_COUNT; p++) {
        const size_t plane_bytes = plane_alloc_bytes(buffer->layout.lines[p],
                                                      buffer->layout.pitch[p]);
        if (plane_bytes > SIZE_MAX - bytes) return SIZE_MAX;
        bytes += plane_bytes;
    }
    return bytes;
}

/* Allocate one layout's three plane buffers. Partial state remains owned by
 * the caller and is released by free_plane_buffer. */
static int alloc_plane_buffer(plane_buffer_t *buffer)
{
    if (plane_buffer_bytes(buffer) == SIZE_MAX) return -1;
    for (int p = 0; p < PLANE_COUNT; p++) {
        size_t bytes = plane_alloc_bytes(buffer->layout.lines[p],
                                         buffer->layout.pitch[p]);
        if (bytes == 0) return -1;
        buffer->data[p] = aligned_alloc(UP_PITCH_ALIGN, bytes);
        if (!buffer->data[p]) return -1;
    }
    return 0;
}

static int alloc_scratch_buffers(zimg_priv_t *p)
{
    /* Each side's scratch is allocated only when that side COPIES. With
     * zero-copy on a side, the graph reads/writes the VLC picture directly and
     * the scratch stays NULL (set per-frame from the VLC picture instead). */
    if (!p->plan.src_zerocopy && alloc_plane_buffer(&p->src) != 0) return -1;
    /* Column tiling gives each worker its own tile dst scratch, so the shared
     * priv-level dst scratch isn't used. */
    if (!p->plan.dst_zerocopy && !p->plan.col_tiled && alloc_plane_buffer(&p->dst) != 0)
        return -1;
    return 0;
}

/*
 * Fill the priv struct's geometry/pitch fields from the scaler context
 * and chroma subsampling. Pure assignment; no allocation.
 */
static void init_priv_geometry(zimg_priv_t *p, const scaler_ctx_t *ctx,
                               unsigned sub_w, unsigned sub_h, int swap)
{
    p->yv12_swap_uv = swap;
    p->sub_w        = sub_w;
    p->sub_h        = sub_h;
    p->src_w = ctx->src_w; p->src_h = ctx->src_h;
    p->dst_w = ctx->dst_w; p->dst_h = ctx->dst_h;

    init_plane_layout(&p->src.layout, ctx->src_w, ctx->src_h, sub_w, sub_h);
    init_plane_layout(&p->dst.layout, ctx->dst_w, ctx->dst_h, sub_w, sub_h);
}

/*
 * Set up one grid worker: stash geometry, build its filter graph, allocate its
 * temporary buffer, connect it to the shared dispatch gate, and spawn the
 * thread. Returns 0 on success, -1 on any failure (caller handles
 * cleanup of partial state via the worker's graph/tmp fields).
 */
/* SCAL-3: allocate this column-tile worker's own dst scratch (tile_dst_w x
 * dst_stripe_h). The graph writes here; worker_copy_out_tile places it into
 * the VLC dst sub-rect. Returns 0 on success, -1 on alloc failure. */
static int alloc_tile_dst(stripe_worker_t *w, const zimg_priv_t *p,
                          int dst_stripe_h)
{
    init_plane_layout(&w->tile_dst.layout, w->cell.dst_w, dst_stripe_h,
                      p->sub_w, p->sub_h);
    if (alloc_plane_buffer(&w->tile_dst) != 0) return -1;
    w->dst = plane_buffer_view(&w->tile_dst);
    return 0;
}

/* Build this cell's zimg graph (full-width source + active_region column crop;
 * tile-width dst) and allocate its tmp buffer. Returns 0, or -1 on build/alloc
 * failure (caller releases via release_worker_resources). */
static int build_worker_graph_and_tmp(stripe_worker_t *w, const zimg_priv_t *p,
                                      const cell_bounds_t *c,
                                      unsigned sub_w, unsigned sub_h,
                                      zimg_resample_filter_e filt)
{
    w->graph = build_stripe_graph(p->src_w, c, sub_w, sub_h, filt);
    if (!w->graph) return -1;
    if (zimg_filter_graph_get_tmp_size(w->graph, &w->tmp_size) != 0) return -1;
    if (w->tmp_size > 0) {
        w->tmp = aligned_alloc(UP_PITCH_ALIGN,
            (w->tmp_size + UP_PITCH_ALIGN - 1) & ~(size_t)(UP_PITCH_ALIGN - 1));
        if (!w->tmp) return -1;
    }
    return 0;
}

static int init_stripe_worker(stripe_worker_t *w, zimg_priv_t *p,
                              int worker_id, const cell_bounds_t *c,
                              unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt)
{
    w->cell.src_y_start = c->rows.src_start;
    w->cell.src_y_end   = c->rows.src_end;
    w->cell.dst_y_start = c->rows.dst_start;
    w->cell.dst_y_end   = c->rows.dst_end;
    w->cell.dst_x_start = c->cols.dst_start;
    w->col_tiled    = p->plan.col_tiled;
    w->cell.src_w       = c->cols.src_end - c->cols.src_start;   /* TILE widths */
    w->cell.dst_w       = c->cols.dst_end - c->cols.dst_start;
    /* worker_main shifts row offsets by sub (`>> w->cell.sub_h`); a value >= the
     * int width would be UB. Valid YUV chroma gives 0 or 1, but clamp both
     * exponents defensively so a malformed value can never reach the shift
     * (UB-2, UB-3). */
    w->cell.sub_h        = (sub_h < 8u) ? sub_h : 0u;
    w->cell.sub_w        = (sub_w < 8u) ? sub_w : 0u;
    /* I/O mode: copy on the side that is NOT zero-copy. */
    w->copy_in      = !p->plan.src_zerocopy;
    w->copy_out     = !p->plan.dst_zerocopy;
    w->gate         = &p->gate;
    w->seen_gen     = p->gate.generation;  /* don't run a frame before the first dispatch */
    w->worker_id    = worker_id;
    w->src = plane_buffer_view(&p->src);

    /* dst buffer: a column tile writes its own tile-sized scratch (owned);
     * otherwise it shares the priv-level dst scratch (or VLC dst in zerocopy,
     * set per-frame). */
    if (w->col_tiled) {
        if (alloc_tile_dst(w, p, c->rows.dst_end - c->rows.dst_start) != 0)
            return -1;
    } else {
        w->dst = plane_buffer_view(&p->dst);
    }

    if (build_worker_graph_and_tmp(w, p, c, sub_w, sub_h, filt) != 0)
        return -1;

    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        return -1;
    }
    w->thread_started = true;
    /* SCAL-4: best-effort pin (opt-in). Round-robin so worker count > core
     * count still spreads evenly; failure is ignored inside the helper. */
    if (p->pin_cpus && p->cpu_topology.pin_count > 0) {
        int pin_index = worker_id % p->cpu_topology.pin_count;
        pin_worker_to_cpu(w->thread, p->cpu_topology.pin_ids[pin_index]);
    }
    return 0;
}

/*
 * Per-worker teardown primitive: release the graph, temporary buffer, tile
 * scratch, and joined thread held by one worker slot. Idempotent — safe on a
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
    if (had_thread) {
        pthread_join(w->thread, NULL);
        w->thread_started = false;
    }
    if (w->graph) { zimg_filter_graph_free(w->graph); w->graph = NULL; }
    free(w->tmp); w->tmp = NULL;
    /* SCAL-3: a column tile owns its dst scratch; the shared (non-tiled) dst
     * is owned by the priv and freed in zimg_close. */
    if (w->col_tiled) free_plane_buffer(&w->tile_dst);
}

/*
 * Tell every started worker to complete any unseen generation, then exit.
 * The exit broadcast does not advance generation: an idle worker must not run
 * stale per-frame state during ordinary teardown.
 */
static void zimg_wake_all_for_exit(zimg_priv_t *p)
{
    /* If the gate never initialized, no thread was ever spawned (construct
     * runs after gate init), so there is nothing to wake. */
    if (!up_pool_gate_ready(&p->gate)) return;
    up_pool_gate_lock(&p->gate);
    for (int i = 0; i < p->plan.n_threads; i++)
        if (p->workers[i].thread_started)
            p->workers[i].should_exit = true;
    up_pool_gate_unlock_broadcast(&p->gate);
}

static void zimg_stop_workers(zimg_priv_t *p)
{
    if (!p->workers) return;
    zimg_wake_all_for_exit(p);
    for (int i = 0; i < p->plan.n_threads; i++) {
        stripe_worker_t *w = &p->workers[i];
        if (!w->thread_started) continue;
        pthread_join(w->thread, NULL);
        w->thread_started = false;
    }
}

/*
 * Tear down all `n` constructed workers and zero their slots so they can
 * be re-initialized cleanly on retry. Used after a partial construction.
 */
static void teardown_constructed_workers(zimg_priv_t *p, int n)
{
    /* One broadcast wakes every started worker (the broadcast reaches all
     * waiters; non-started slots are skipped), THEN join — same
     * signal-then-reap order as zimg_close. */
    zimg_stop_workers(p);
    for (int i = 0; i < n; i++) {
        stripe_worker_t *w = &p->workers[i];
        release_worker_resources(w, w->thread_started);
        memset(w, 0, sizeof *w);
    }
}

/*
 * Spawn the worker for cell index `i` of the n_rows x n_cols grid (SCAL-3):
 * row = i / n_cols owns a height stripe, col = i % n_cols owns a width tile.
 * up_compute_stripe_bounds() partitions each axis (even-aligned, so column
 * boundaries stay chroma-exact). Returns 0 on success, -1 on degenerate
 * geometry or worker init failure (partial graph/tmp/tile-dst released here).
 */
static int try_spawn_one_worker(zimg_priv_t *p, int i,
                                unsigned sub_w, unsigned sub_h,
                                zimg_resample_filter_e filt)
{
    stripe_worker_t *w = &p->workers[i];
    const int row = i / p->plan.n_cols;
    const int col = i % p->plan.n_cols;

    cell_bounds_t c;
    if (!up_compute_stripe_bounds(row, p->plan.n_rows, p->src_h, p->dst_h,
                                  &c.rows))
        return -1;
    if (!up_compute_stripe_bounds(col, p->plan.n_cols, p->src_w, p->dst_w,
                                  &c.cols))
        return -1;

    if (init_stripe_worker(w, p, i, &c, sub_w, sub_h, filt) != 0) {
        release_worker_resources(w, false);   /* no thread started yet */
        return -1;
    }
    return 0;
}

/*
 * Construct all n_rows*n_cols grid cells. The grid (up_decide_tile_grid) makes
 * every cell >= stripe_min rows and >= col_min cols, so geometry never
 * degenerates. Allocation, graph build, or thread creation can fail. This pool
 * deliberately builds all-or-nothing rather than retrying a smaller grid: on any
 * cell failure, tear down the cells already built and report 0. The last row/
 * col always ends at dst_h/dst_w, so a full build covers the frame exactly.
 */
static void construct_workers(zimg_priv_t *p,
                              unsigned sub_w, unsigned sub_h,
                              zimg_resample_filter_e filt,
                              int *out_constructed)
{
    for (int i = 0; i < p->plan.n_threads; i++) {
        if (try_spawn_one_worker(p, i, sub_w, sub_h, filt) != 0) {
            teardown_constructed_workers(p, i);   /* tear down [0, i) */
            *out_constructed = 0;
            return;
        }
    }
    *out_constructed = p->plan.n_threads;
}

/* Diagnostic log emitted once after successful lazy initialization. */
static void log_zimg_open(vlc_object_t *log_obj, const zimg_priv_t *p)
{
    if (!log_obj) return;
    /* Only scratch that is actually allocated (the copy side) counts. */
    size_t src_mb = p->plan.src_zerocopy ? 0 : (plane_buffer_bytes(&p->src) >> 20);
    /* Column tiling uses per-worker tile dst scratch, not the shared p->dst. */
    size_t dst_mb = (p->plan.dst_zerocopy || p->plan.col_tiled) ? 0
        : (plane_buffer_bytes(&p->dst) >> 20);
    /* OBS-5: every cell owns a persistent zimg graph tmp buffer; at high thread
     * counts this dominates the reported scratch, so account for it here. */
    size_t tmp_bytes = 0;
    for (int i = 0; i < p->plan.n_threads; i++)
        tmp_bytes += p->workers[i].tmp_size;
    msg_Info(log_obj,
             "zimg: %d worker thread%s (grid %dx%d), %dx%d -> %dx%d, "
             "scratch %zu MB (src %s, dst %s), graph-tmp %zu MB",
             p->plan.n_threads, p->plan.n_threads == 1 ? "" : "s",
             p->plan.n_rows, p->plan.n_cols,
             p->src_w, p->src_h, p->dst_w, p->dst_h,
             src_mb + dst_mb,
             p->plan.src_zerocopy ? "zero-copy" : "copy",
             p->plan.col_tiled ? "tiled+copy" : (p->plan.dst_zerocopy ? "zero-copy" : "copy"),
             tmp_bytes >> 20);
}

/*
 * Lazy initialization of the worker pool, scratch buffers, and per-cell
 * filter graphs. Called on the first zimg_process() invocation rather
 * than from zimg_open().
 *
 * Why defer this? VLC's filter-chain solver instantiates filters
 * speculatively while searching for a working chain. With hardware
 * decode + a non-trivial filter chain (e.g. postproc + autoupscale),
 * the solver may construct and tear down filters speculatively before
 * settling on a working configuration. Spawning workers and allocating full
 * scratch for those probes is wasteful. Doing it lazily means VLC pays
 * nothing for probes that
 * never produce a frame; only the first valid Filter() picture triggers
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
        size_t total = (size_t)p->plan.n_threads * sizeof(*p->workers);
        p->workers = aligned_alloc(64, total);
        if (!p->workers) return -1;
        memset(p->workers, 0, total);
    }

    /* Gate must be live before construct_workers spawns threads, since
     * each worker blocks on the cv immediately (seen_gen == generation). */
    if (up_pool_gate_init(&p->gate) != 0) return -1;

    int constructed = 0;
    /* ARCH-1: the worker-construction chain reads only src/dst geometry, all
     * of which lives in the priv struct (init_priv_geometry) — no fake ctx. */
    construct_workers(p, p->sub_w, p->sub_h,
                      (zimg_resample_filter_e)p->lazy.algo, &constructed);
    if (constructed == 0) return -1;

    log_zimg_open((vlc_object_t *)p->lazy.log_obj, p);
    return 0;
}

/*
 * zimg_open: cheap setup only. Validates that we can handle the input
 * chroma, computes geometry and thread count, allocates the priv struct,
 * and returns. Does NOT spawn workers, allocate scratch, or build
 * per-cell graphs - that all happens lazily on the first valid Filter()
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

    up_cpu_topology_t cpu_topology;
    up_detect_cpu_topology(&cpu_topology);
    int n_threads = up_threads_decide(ctx->threads_pref,
                                      cpu_topology.allowed_count);

    /* SCAL-3/PAT-1: resolve grid + zero-copy modes in one place. Storage
     * alignment is unknown until the first frame, so the raw options go
     * in; zimg_prepare_first_frame_io re-resolves once with alignment. */
    int stripe_min_lines = up_zimg_stripe_min_lines(ctx->zimg.min_stripe_lines);
    const up_zimg_io_req_t req = {
        .worker_budget = n_threads,
        .dst_w         = ctx->dst_w,
        .dst_h         = ctx->dst_h,
        .stripe_min    = stripe_min_lines,
        .col_min       = ZIMG_COL_MIN_WIDTH,
        .src_zerocopy  = (ctx->zimg.src_zerocopy != 0),
        .dst_zerocopy  = (ctx->zimg.zerocopy != 0),
    };
    up_zimg_io_plan_t plan;
    up_zimg_resolve_io_plan(&req, &plan);

    zimg_priv_t *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->worker_budget = n_threads;
    p->plan = plan;
    init_priv_geometry(p, ctx, sub_w, sub_h, swap);

    /* Save what zimg_lazy_init() needs that isn't already in priv. */
    p->lazy.algo    = (int)AlgoToZimg(ctx->algo);
    p->lazy.log_obj = ctx->log_obj;

    if (!req.src_zerocopy && ctx->log_obj) {
        /* Would the grid have tiled with source-direct reads? Then
         * zerocopy-src=0 is what demoted it — say so. */
        up_zimg_io_req_t hyp = req;
        up_zimg_io_plan_t tiled;
        hyp.src_zerocopy = true;
        up_zimg_resolve_io_plan(&hyp, &tiled);
        if (tiled.col_tiled)
            msg_Warn((vlc_object_t *)ctx->log_obj,
                     "AutoUpscale: zerocopy-src=0 disables column tiling; "
                     "using %d row stripes instead of %dx%d grid",
                     plan.n_rows, tiled.n_rows, tiled.n_cols);
    }

    /* SCAL-4/5: retain the same allowed topology used for thread planning so
     * sparse cpusets are pinned correctly and capacity cannot drift. */
    p->pin_cpus = (ctx->pin_cpus != 0);
    p->cpu_topology = cpu_topology;

    ctx->priv = p;
    return 0;
}

/* Which of the worker's four plane views a VLC picture lands in, per
 * side and zero-copy mode (CPLX-1: replaces offsetof selectors):
 *   src      ← VLC src  when src zero-copy (graph reads VLC src)
 *   vlc_src  ← VLC src  when copy-in       (worker copies VLC src -> scratch)
 *   dst      ← VLC dst  when dst zero-copy  (graph writes VLC dst)
 *   vlc_dst  ← VLC dst  when copy-out       (worker copies scratch -> VLC dst)
 */
typedef enum { WORKER_VIEW_SRC, WORKER_VIEW_DST } worker_view_side_e;

static plane_view_t *worker_view_for(stripe_worker_t *w,
                                     worker_view_side_e side, bool zerocopy)
{
    if (side == WORKER_VIEW_SRC)
        return zerocopy ? &w->src : &w->vlc_src;
    return zerocopy ? &w->dst : &w->vlc_dst;
}

/*
 * Per-frame: copy one VLC picture's plane pointers + pitches into the
 * selected `plane_view_t` of every worker. Drives all four per-frame
 * pointer-sets through one loop (DUP-7/PERF-6). Workers are blocked on
 * the gate while this runs, so the writes need no synchronization.
 * YV12 U/V are swapped via the plane-index map.
 */
static void point_workers_planes(zimg_priv_t *p,
                                  const up_picture_view_t *pic,
                                  worker_view_side_e side, bool zerocopy)
{
    const int swap = p->yv12_swap_uv;
    const int iy = 0;
    const int iu = up_zimg_plane_idx(1, swap);
    const int iv = up_zimg_plane_idx(2, swap);
    for (int i = 0; i < p->plan.n_threads; i++) {
        plane_view_t *view = worker_view_for(&p->workers[i], side, zerocopy);
        view->data[PLANE_Y] = pic->plane[iy].pixels;
        view->data[PLANE_U] = pic->plane[iu].pixels;
        view->data[PLANE_V] = pic->plane[iv].pixels;
        view->pitch[PLANE_Y] = pic->plane[iy].pitch;
        view->pitch[PLANE_U] = pic->plane[iu].pitch;
        view->pitch[PLANE_V] = pic->plane[iv].pitch;
    }
}

/*
 * Dispatch all workers and wait for completion.
 * A barrier or worker failure poisons the backend, so both are fatal.
 */
static scaler_process_status_t zimg_dispatch_and_wait(zimg_priv_t *p)
{
    /* Wake side: arm the barrier and reset results inside the gate's
     * critical section, so a worker that wakes sees pending already
     * armed and its result already reset. */
    up_pool_gate_lock(&p->gate);
    up_pool_gate_arm_locked(&p->gate, p->plan.n_threads);
    for (int i = 0; i < p->plan.n_threads; i++)
        p->workers[i].result = 0;
    up_pool_gate_unlock_broadcast(&p->gate);

    /* On a fatal barrier error, synchronously drain the dispatched generation
     * and join the pool before returning control to the picture owner.
     * OBS-1: log once — pool_broken latches, so zimg_process short-circuits
     * every later frame and this site is not reached again. */
    if (up_pool_gate_wait_all(&p->gate) != 0) {
        p->pool_broken = true;
        if (p->lazy.log_obj)
            msg_Err((vlc_object_t *)p->lazy.log_obj,
                    "AutoUpscale: zimg worker completion barrier failed; "
                    "pool poisoned (this is logged only once)");
        zimg_stop_workers(p);
        return SCALER_PROCESS_FATAL;
    }
    for (int i = 0; i < p->plan.n_threads; i++) {
        if (p->workers[i].result != 0) {
            p->pool_broken = true;
            if (p->lazy.log_obj)
                msg_Err((vlc_object_t *)p->lazy.log_obj,
                        "AutoUpscale: zimg graph processing failed on worker "
                        "%d (libzimg error %d); pool poisoned (logged once)",
                        i, (int)p->workers[i].err_code);
            return SCALER_PROCESS_FATAL;
        }
    }
    return SCALER_PROCESS_OK;
}

/*
 * Lazy init on the first valid frame: spawn workers, allocate scratch, and
 * build per-cell graphs — done once so VLC can probe us cheaply during chain
 * setup. Returns 0 if ready (already or just initialized), -1 on sticky
 * failure. Extracted to keep zimg_process within the complexity limit.
 */
static int zimg_ensure_lazy_init(zimg_priv_t *p)
{
    if (p->lazy.done) return 0;
    if (p->lazy.failed) return -1;
    if (zimg_lazy_init(p) != 0) {
        p->lazy.failed = true;
        /* OBS-2: the expensive setup ran at the first valid frame, after cheap
         * Open() succeeded; say so once, else every frame drops silently. */
        if (p->lazy.log_obj)
            msg_Err((vlc_object_t *)p->lazy.log_obj,
                    "zimg: lazy backend init failed (%dx%d -> %dx%d); "
                    "dropping this frame (AUTO may fall back)",
                    p->src_w, p->src_h, p->dst_w, p->dst_h);
        return -1;
    }
    p->lazy.done = true;
    return 0;
}

/* zimg requires every direct image address and stride to be 32-byte aligned.
 * Cropping can move an otherwise aligned VLC allocation off that boundary. */
static bool zimg_view_aligned(const up_picture_view_t *view)
{
    for (int i = 0; i < view->plane_count; i++)
        if ((uintptr_t)view->plane[i].pixels % ZIMG_BUFFER_ALIGN != 0
                || view->plane[i].pitch % ZIMG_BUFFER_ALIGN != 0)
            return false;
    return true;
}

/* Lazy init lets the first real picture select the safe I/O mode. If a VLC
 * crop breaks zimg's direct-buffer alignment contract, use the aligned copy
 * buffers. Source copy-in cannot use column graphs, so fall back to the same
 * rows-only grid used by the explicit zerocopy-src=0 option. */
static void zimg_prepare_first_frame_io(zimg_priv_t *p,
                                        const scaler_ctx_t *ctx,
                                        const up_picture_view_t *src,
                                        const up_picture_view_t *dst)
{
    if (p->lazy.done || p->lazy.failed) return;
    /* Re-resolve the plan with real storage alignment folded into the
     * requested flags; the resolver is a fixed point, so an unchanged
     * request yields the identical plan. */
    const up_zimg_io_req_t req = {
        .worker_budget = p->worker_budget,
        .dst_w         = p->dst_w,
        .dst_h         = p->dst_h,
        .stripe_min    = up_zimg_stripe_min_lines(ctx->zimg.min_stripe_lines),
        .col_min       = ZIMG_COL_MIN_WIDTH,
        .src_zerocopy  = (ctx->zimg.src_zerocopy != 0)
                         && zimg_view_aligned(src),
        .dst_zerocopy  = (ctx->zimg.zerocopy != 0)
                         && zimg_view_aligned(dst),
    };
    up_zimg_io_plan_t plan;
    up_zimg_resolve_io_plan(&req, &plan);
    p->plan = plan;
}

/* A later frame may drift to different storage. Copy paths accept arbitrary
 * valid VLC alignment; an already-built zero-copy graph does not. */
static bool zimg_frame_io_safe(const zimg_priv_t *p,
                               const up_picture_view_t *src,
                               const up_picture_view_t *dst)
{
    return (!p->plan.src_zerocopy || zimg_view_aligned(src))
        && (!p->plan.dst_zerocopy || zimg_view_aligned(dst));
}

static bool zimg_frame_views_init(const scaler_ctx_t *ctx,
                                  const picture_t *src, const picture_t *dst,
                                  up_picture_view_t *src_view,
                                  up_picture_view_t *dst_view)
{
    const up_picture_region_t src_region = up_scaler_src_region(ctx);
    const up_picture_region_t dst_region = up_scaler_dst_region(ctx);
    return up_picture_view_init(src_view, src, ctx->chroma, &src_region)
        && up_picture_view_init(dst_view, dst, ctx->chroma, &dst_region);
}

static void zimg_warn_bad_geometry(zimg_priv_t *p)
{
    if (p->preflight_warned) return;
    p->preflight_warned = true;
    if (p->lazy.log_obj)
        msg_Warn((vlc_object_t *)p->lazy.log_obj,
                 "zimg: source/destination picture geometry unusable "
                 "(planes, extent, or crop); dropping frame(s)");
}

/* REL-6: zero-copy graphs are built for the first frame's storage
 * alignment; a drifted later frame is dropped (TRANSIENT). A persistent
 * streak means the allocator changed for good — without escalation every
 * remaining frame would drop silently, since TRANSIENT never engages the
 * swscale fallback. Warn once, and after a full streak return FATAL so
 * the fallback (which accepts any valid alignment) can take over. */
#define ZIMG_DRIFT_FATAL_STREAK 30

static scaler_process_status_t zimg_note_alignment_drift(zimg_priv_t *p)
{
    if (!p->drift.warned) {
        p->drift.warned = true;
        if (p->lazy.log_obj)
            msg_Warn((vlc_object_t *)p->lazy.log_obj,
                     "AutoUpscale: zimg: picture storage drifted from the "
                     "alignment the zero-copy graphs were built for; "
                     "dropping frame(s), failing over after %d consecutive "
                     "misses", ZIMG_DRIFT_FATAL_STREAK);
    }
    if (++p->drift.streak >= ZIMG_DRIFT_FATAL_STREAK) {
        p->pool_broken = true;
        return SCALER_PROCESS_FATAL;
    }
    return SCALER_PROCESS_TRANSIENT;
}

static scaler_process_status_t zimg_process(scaler_ctx_t *ctx,
                                            const picture_t *src,
                                            const picture_t *dst)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return SCALER_PROCESS_FATAL;
    if (p->pool_broken) return SCALER_PROCESS_FATAL;

    up_picture_view_t src_view, dst_view;
    if (!zimg_frame_views_init(ctx, src, dst, &src_view, &dst_view)) {
        zimg_warn_bad_geometry(p);
        return SCALER_PROCESS_TRANSIENT;
    }
    zimg_prepare_first_frame_io(p, ctx, &src_view, &dst_view);
    if (!zimg_frame_io_safe(p, &src_view, &dst_view))
        return zimg_note_alignment_drift(p);
    p->drift.streak = 0;
    if (zimg_ensure_lazy_init(p) != 0) return SCALER_PROCESS_FATAL;

    /* Per-frame plane pointers. On each side the graph touches the VLC
     * picture directly (zero-copy) or the workers copy via scratch. */
    point_workers_planes(p, &src_view, WORKER_VIEW_SRC, p->plan.src_zerocopy);
    point_workers_planes(p, &dst_view, WORKER_VIEW_DST, p->plan.dst_zerocopy);

    return zimg_dispatch_and_wait(p);
}

static void zimg_close(scaler_ctx_t *ctx)
{
    zimg_priv_t *p = ctx->priv;
    if (!p) return;

    if (p->workers) {
        zimg_stop_workers(p);
        for (int i = 0; i < p->plan.n_threads; i++) {
            stripe_worker_t *w = &p->workers[i];
            release_worker_resources(w, w->thread_started);
        }
        free(p->workers);
    }
    up_pool_gate_destroy(&p->gate);

    free_plane_buffer(&p->src);
    free_plane_buffer(&p->dst);
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
