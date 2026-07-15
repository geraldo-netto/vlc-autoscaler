// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler.h — pluggable scaler backend interface for AutoUpscale
 *****************************************************************************
 * Two backends: zimg (preferred, high-quality, supports Spline36) and
 * swscale (broad-coverage fallback). Either backend may decline a particular
 * (chroma, algo) combination via supports(); scaler_pick() handles
 * support-based selection and the plugin owns open/runtime fallback.
 *
 * Backends are plain C structs of function pointers. The zimg backend is
 * compiled in iff HAVE_ZIMG is defined at build time (driven by pkg-config).
 *****************************************************************************/

#ifndef AUTOUPSCALE_SCALER_H
#define AUTOUPSCALE_SCALER_H

#include <vlc_common.h>
#include <vlc_picture.h>

#include "picture_view.h"
#include "scaler_status.h"

/* User-visible backend preference (do NOT renumber). */
#define SCALER_BACKEND_AUTO     0
#define SCALER_BACKEND_ZIMG     1
#define SCALER_BACKEND_SWSCALE  2
#define SCALER_BACKEND_MAX      SCALER_BACKEND_SWSCALE

typedef struct scaler_backend_s scaler_backend_t;

typedef struct scaler_ctx_s
{
    const scaler_backend_t *backend;
    void                   *priv;       /* backend-private state */
    int                     src_w;
    int                     src_h;
    /* VLC 3 pictures do not reliably retain the negotiated crop metadata. */
    unsigned                src_coded_w;
    unsigned                src_coded_h;
    unsigned                src_x_offset;
    unsigned                src_y_offset;
    int                     dst_w;
    int                     dst_h;
    int                     algo;
    int                     threads_pref; /* 0 = auto; >0 = explicit */
    int                     pin_cpus;   /* SCAL-4: 1 = pin each scaler worker
                                         * thread to a distinct CPU core (opt-in,
                                         * Linux only). 0 = let the scheduler
                                         * place threads (default). */

    /* Backend-specific tunables. Only the zimg backend reads these;
     * swscale ignores them. Grouped under a named member so the generic
     * geometry above stays free of backend coupling (ISP). A third
     * backend would add its own sibling member here. */
    struct {
        int min_stripe_lines; /* 0 = use default 16; smaller lets more workers
                               * fit at the cost of dispatch/boundary overhead */
        int zerocopy;         /* 1 = write directly to VLC dst picture
                               * (default); 0 = copy-out via scratch
                               * (safety fallback) */
        int src_zerocopy;     /* 1 = worker graphs read VLC src picture
                               * directly (default); 0 = copy-in to
                               * scratch (safety fallback) */
    }                       zimg;

    vlc_fourcc_t            chroma;     /* same on input and output */
    vlc_object_t           *log_obj;    /* backend diagnostics */
} scaler_ctx_t;

static inline up_picture_region_t
up_scaler_src_region(const scaler_ctx_t *ctx)
{
    return (up_picture_region_t) {
        .coded_width = ctx->src_coded_w,
        .coded_height = ctx->src_coded_h,
        .x_offset = ctx->src_x_offset,
        .y_offset = ctx->src_y_offset,
        .width = ctx->src_w,
        .height = ctx->src_h,
    };
}

static inline up_picture_region_t
up_scaler_dst_region(const scaler_ctx_t *ctx)
{
    return (up_picture_region_t) {
        .coded_width = ctx->dst_w,
        .coded_height = ctx->dst_h,
        .width = ctx->dst_w,
        .height = ctx->dst_h,
    };
}

struct scaler_backend_s
{
    const char *name;

    /* Stable identity, one of SCALER_BACKEND_ZIMG / _SWSCALE. Lets callers
     * branch on which backend is active without sniffing `name` (the old
     * `name[0] != 's'` test was a brittle proxy for "not swscale"). */
    int         id;

    /* Returns 1 if this backend can handle (chroma, algo), 0 otherwise.
     * Pure function: no allocations, safe to call before open(). */
    int  (*supports)( vlc_fourcc_t chroma, int algo );

    /* Allocate and initialise priv. ctx fields (src/dst dims, algo,
     * chroma) must already be set. Returns 0 on success, -1 on failure;
     * failure must leave ctx->priv NULL so AUTO can try another backend. */
    int  (*open)   ( scaler_ctx_t *ctx );

    /* Process one frame. TRANSIENT drops only this frame; FATAL means the
     * backend is unusable and the caller may replace it. */
    scaler_process_status_t (*process)( scaler_ctx_t *ctx,
                                        const picture_t *src,
                                        const picture_t *dst );

    /* Tear down priv. Always safe to call after open() success. */
    void (*close)  ( scaler_ctx_t *ctx );
};

/*
 * Pick the best backend matching `pref` that can handle (chroma, algo).
 *   pref = SCALER_BACKEND_AUTO     : prefer zimg, fall back to swscale
 *   pref = SCALER_BACKEND_ZIMG     : zimg only (NULL if not built/supported)
 *   pref = SCALER_BACKEND_SWSCALE  : swscale only
 * Returns NULL if no backend can handle the request.
 */
const scaler_backend_t *scaler_pick( int pref,
                                     vlc_fourcc_t chroma, int algo );

#endif /* AUTOUPSCALE_SCALER_H */
