/*****************************************************************************
 * scaler_swscale.c — libswscale backend for the scaler interface
 *****************************************************************************
 * Universal fallback. Handles every chroma the plugin supports. Fast and
 * well-tested but single-threaded per frame and slightly lower quality
 * than zimg's Spline36 on edges.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"
#include "upscale_logic.h"

#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct
{
    struct SwsContext *ctx;
    enum AVPixelFormat av_fmt;
} sws_priv_t;

/* Local helper — same set of chromas the original plugin supported. */
static enum AVPixelFormat ChromaToAVFmt( vlc_fourcc_t c )
{
    switch( c )
    {
        case VLC_CODEC_I420:
        case VLC_CODEC_YV12:  return AV_PIX_FMT_YUV420P;
        case VLC_CODEC_NV12:  return AV_PIX_FMT_NV12;
        case VLC_CODEC_NV21:  return AV_PIX_FMT_NV21;
        case VLC_CODEC_I422:  return AV_PIX_FMT_YUV422P;
        case VLC_CODEC_I444:  return AV_PIX_FMT_YUV444P;
        case VLC_CODEC_RGB24: return AV_PIX_FMT_RGB24;
        case VLC_CODEC_RGBA:  return AV_PIX_FMT_RGBA;
        case VLC_CODEC_BGRA:  return AV_PIX_FMT_BGRA;
        default:              return AV_PIX_FMT_NONE;
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

static int sws_process( scaler_ctx_t *ctx,
                        const picture_t *src, picture_t *dst )
{
    sws_priv_t *p = ctx->priv;
    if( !p ) return -1;

    const uint8_t *src_data[4]   = { NULL };
    int            src_stride[4] = { 0 };
    uint8_t       *dst_data[4]   = { NULL };
    int            dst_stride[4] = { 0 };

    for( int i = 0; i < src->i_planes && i < 4; i++ )
    {
        src_data[i]   = src->p[i].p_pixels;
        src_stride[i] = src->p[i].i_pitch;
    }
    for( int i = 0; i < dst->i_planes && i < 4; i++ )
    {
        dst_data[i]   = dst->p[i].p_pixels;
        dst_stride[i] = dst->p[i].i_pitch;
    }

    int rc = sws_scale( p->ctx, src_data, src_stride, 0, ctx->src_h,
                        dst_data, dst_stride );
    return rc > 0 ? 0 : -1;
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
    .supports = sws_supports,
    .open     = sws_open,
    .process  = sws_process,
    .close    = sws_close,
};
