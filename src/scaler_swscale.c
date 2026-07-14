// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_swscale.c — libswscale backend for the scaler interface
 *****************************************************************************
 * Broad-coverage fallback. Handles every chroma the plugin supports. Fast and
 * well-tested but single-threaded per frame and slightly lower quality
 * than zimg's Spline36 on edges.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"
#include "chroma_classify.h"
#include "picture_view.h"
#include "upscale_logic.h"

#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct
{
    struct SwsContext *ctx;
    enum AVPixelFormat av_fmt;
    /* OBS-3: one-shot latches so a persistently bad stream logs a reason once
     * instead of spamming every frame — symmetric with zimg's bad-geometry
     * warning. */
    int                geom_warned;
    int                short_warned;
} sws_priv_t;

/* OBS-3: emit a one-shot swscale failure reason, latched via *warned. */
static void sws_warn_once( const scaler_ctx_t *ctx, int *warned,
                           const char *reason )
{
    if( !ctx->log_obj || *warned ) return;
    *warned = 1;
    msg_Warn( ctx->log_obj, "swscale: %s (logged only once)", reason );
}

static enum AVPixelFormat ChromaToAVFmt( vlc_fourcc_t c )
{
    const up_chroma_descriptor_t *desc =
        up_chroma_descriptor( (uint32_t)c );
    if( !desc ) return AV_PIX_FMT_NONE;
    switch( desc->layout )
    {
        case UP_CHROMA_LAYOUT_YUV420P: return AV_PIX_FMT_YUV420P;
        case UP_CHROMA_LAYOUT_YUV422P: return AV_PIX_FMT_YUV422P;
        case UP_CHROMA_LAYOUT_YUV444P: return AV_PIX_FMT_YUV444P;
        case UP_CHROMA_LAYOUT_NV12:    return AV_PIX_FMT_NV12;
        case UP_CHROMA_LAYOUT_NV21:    return AV_PIX_FMT_NV21;
        case UP_CHROMA_LAYOUT_RGB24:   return AV_PIX_FMT_RGB24;
        case UP_CHROMA_LAYOUT_RGBA:    return AV_PIX_FMT_RGBA;
        case UP_CHROMA_LAYOUT_BGRA:    return AV_PIX_FMT_BGRA;
        default:                       return AV_PIX_FMT_NONE;
    }
}

static int AlgoToSwsFlags( int algo )
{
    /* Quality flags layered on top of the algorithm. */
    int extra = SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    switch( algo )
    {
        case UP_ALGO_FAST_BILINEAR: return SWS_FAST_BILINEAR | extra;
        case UP_ALGO_BICUBIC:       return SWS_BICUBIC       | extra;
        case UP_ALGO_LANCZOS:       return SWS_LANCZOS       | extra;
        case UP_ALGO_SPLINE36:
            /* swscale has no Spline36 — closest match is Lanczos. */
        default:                    return SWS_LANCZOS       | extra;
    }
}

/* ---------- backend ops ---------- */

static int sws_supports( vlc_fourcc_t chroma, int algo )
{
    (void)algo;  /* All UP_ALGO_* values are handled (Spline36 -> Lanczos). */
    return ChromaToAVFmt( chroma ) != AV_PIX_FMT_NONE;
}

static int sws_open( scaler_ctx_t *ctx )
{
    enum AVPixelFormat fmt = ChromaToAVFmt( ctx->chroma );
    if( fmt == AV_PIX_FMT_NONE ) return -1;

    sws_priv_t *p = calloc( 1, sizeof(*p) );
    if( !p ) return -1;
    p->av_fmt = fmt;

    p->ctx = sws_getContext(
        ctx->src_w, ctx->src_h, fmt,
        ctx->dst_w, ctx->dst_h, fmt,
        AlgoToSwsFlags( ctx->algo ),
        NULL, NULL, NULL );

    if( !p->ctx ) { free(p); return -1; }

    if( ctx->algo == UP_ALGO_SPLINE36 && ctx->log_obj )
        msg_Dbg( ctx->log_obj,
                 "swscale: Spline36 unavailable, using Lanczos" );

    ctx->priv = p;
    return 0;
}

/* libswscale's YUV420P descriptor expects semantic Y/U/V, while VLC stores
 * YV12 physically as Y/V/U. Other formats keep their physical plane order. */
static int sws_plane_index( const up_chroma_descriptor_t *desc, int plane )
{
    if( !desc || !desc->uv_planes_swapped || plane == 0 )
        return plane;
    return plane == 1 ? 2 : 1;
}

/* dst is the output frame. The picture_t itself is only read; the pixel
 * storage it points to is written through the plane view, so dst is const. */
static scaler_process_status_t sws_process( scaler_ctx_t *ctx,
                                            const picture_t *src,
                                            const picture_t *dst )
{
    sws_priv_t *p = ctx->priv;
    if( !p ) return SCALER_PROCESS_FATAL;
    const up_chroma_descriptor_t *desc =
        up_chroma_descriptor( (uint32_t)ctx->chroma );
    if( !desc ) return SCALER_PROCESS_FATAL;

    const up_picture_region_t src_region = up_scaler_src_region(ctx);
    const up_picture_region_t dst_region = up_scaler_dst_region(ctx);
    up_picture_view_t src_view;
    up_picture_view_t dst_view;
    if( !up_picture_view_init( &src_view, src, ctx->chroma, &src_region )
     || !up_picture_view_init( &dst_view, dst, ctx->chroma, &dst_region ) )
    {
        sws_warn_once( ctx, &p->geom_warned,
                       "frame geometry unusable (crop/stride/subsample "
                       "mismatch); dropping frame(s)" );
        return SCALER_PROCESS_TRANSIENT;
    }

    const uint8_t *src_data[4]   = { NULL };
    int            src_stride[4] = { 0 };
    uint8_t       *dst_data[4]   = { NULL };
    int            dst_stride[4] = { 0 };

    for( int i = 0; i < src_view.plane_count && i < 4; i++ )
    {
        const int plane = sws_plane_index( desc, i );
        src_data[i]   = src_view.plane[plane].pixels;
        src_stride[i] = src_view.plane[plane].pitch;
    }
    for( int i = 0; i < dst_view.plane_count && i < 4; i++ )
    {
        const int plane = sws_plane_index( desc, i );
        dst_data[i]   = dst_view.plane[plane].pixels;
        dst_stride[i] = dst_view.plane[plane].pitch;
    }

    int rc = sws_scale( p->ctx, src_data, src_stride, 0, ctx->src_h,
                        dst_data, dst_stride );
    /* sws_scale returns the number of output lines written. A full-frame
     * scale must emit exactly dst_h lines; a short return (rc < dst_h, incl.
     * 0 or a negative error) leaves the destination partially filled, so
     * fail rather than forward a half-written frame. */
    if( rc < ctx->dst_h )
        sws_warn_once( ctx, &p->short_warned,
                       "sws_scale emitted fewer lines than the target height; "
                       "dropping frame(s)" );
    return scaler_process_lines_status( rc, ctx->dst_h );
}

static void sws_close( scaler_ctx_t *ctx )
{
    sws_priv_t *p = ctx->priv;
    if( p )
    {
        if( p->ctx ) sws_freeContext( p->ctx );
        free( p );
        ctx->priv = NULL;
    }
}

const scaler_backend_t scaler_backend_swscale_impl = {
    .name     = "swscale",
    .id       = SCALER_BACKEND_SWSCALE,
    .supports = sws_supports,
    .open     = sws_open,
    .process  = sws_process,
    .close    = sws_close,
};
