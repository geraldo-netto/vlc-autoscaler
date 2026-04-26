// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler.h — pluggable scaler backend interface for AutoUpscale
 *****************************************************************************
 * Two backends: zimg (preferred, high-quality, supports Spline36) and
 * swscale (universal fallback). Either backend may decline a particular
 * (chroma, algo) combination via supports(); scaler_pick() handles
 * failover.
 *
 * Backends are plain C structs of function pointers. The zimg backend is
 * compiled in iff HAVE_ZIMG is defined at build time (driven by pkg-config).
 *****************************************************************************/

#ifndef AUTOUPSCALE_SCALER_H
#define AUTOUPSCALE_SCALER_H

#include <vlc_common.h>
#include <vlc_picture.h>

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
    int                     src_w, src_h;
    int                     dst_w, dst_h;
    int                     algo;       /* UP_ALGO_* */
    int                     threads_pref; /* 0 = auto; >0 = explicit */
    int                     dst_zerocopy; /* 0 = copy-out (safe); 1 = write
                                           * directly to VLC dst picture
                                           * (opt-in, may not work everywhere) */
    vlc_fourcc_t            chroma;     /* same on input and output */
    vlc_object_t           *log_obj;    /* for msg_Dbg/msg_Warn */
} scaler_ctx_t;

struct scaler_backend_s
{
    const char *name;

    /* Returns 1 if this backend can handle (chroma, algo), 0 otherwise.
     * Pure function: no allocations, safe to call before open(). */
    int  (*supports)( vlc_fourcc_t chroma, int algo );

    /* Allocate and initialise priv. ctx fields (src/dst dims, algo,
     * chroma) must already be set. Returns 0 on success, -1 on failure. */
    int  (*open)   ( scaler_ctx_t *ctx );

    /* Process one frame. Returns 0 on success, -1 on failure. */
    int  (*process)( scaler_ctx_t *ctx,
                     const picture_t *src, picture_t *dst );

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
