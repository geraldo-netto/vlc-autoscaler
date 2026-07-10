// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * zimg_test_util.h — construct bare picture_t objects for the zimg backend
 *****************************************************************************
 * The zimg backend (src/scaler_zimg.c) is VLC-typed but does NOT touch VLC's
 * picture pool, refcounting, or logging at runtime (it only reads/writes
 * p[k].p_pixels / i_pitch and skips all msg_* when log_obj == NULL). That
 * lets a test/bench build its own plain picture_t with malloc'd planes and
 * drive backend->open()/process()/close() directly — no running VLC.
 *
 * Shared by tests/test_scaler_zimg.c and tests/bench_scaler_zimg.c. Requires
 * VLC + libzimg headers (built only when HAVE_ZIMG).
 *****************************************************************************/
#ifndef ZIMG_TEST_UTIL_H
#define ZIMG_TEST_UTIL_H

#include "../src/scaler.h"
#include "../src/scaler_zimg_chroma.h"
#include "../src/zimg_helpers.h"
#include "../src/upscale_logic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern const scaler_backend_t scaler_backend_zimg_impl;

/* msg_* macros reference this symbol; the plugin normally provides it. The
 * test never actually logs (log_obj is NULL), but the reference must link. */
#ifdef ZIMG_TEST_DEFINE_MODULE_NAME
const char vlc_module_name[] = "test_scaler_zimg";
#endif

#define ZT_ALIGN 64

typedef struct {
    picture_t pic;
    uint8_t  *buf[3];
} zt_pic_t;

static inline int zt_align_up(int v)
{
    return (v + (ZT_ALIGN - 1)) & ~(ZT_ALIGN - 1);
}

/* Allocate a 3-plane YUV picture for `chroma` at w x h. Pitch is 64-aligned;
 * chroma planes are subsampled per the chroma. Returns 0 on success, -1 if
 * the chroma is unsupported or any allocation fails. CCN 4. */
static inline int zt_pic_alloc(zt_pic_t *tp, uint32_t chroma, int w, int h)
{
    unsigned sw, sh;
    int swap;
    if (!up_chroma_to_zimg(chroma, &sw, &sh, &swap)) return -1;

    memset(tp, 0, sizeof *tp);
    tp->pic.i_planes = 3;
    tp->pic.format.i_chroma = chroma;
    tp->pic.format.i_width = (unsigned)w;
    tp->pic.format.i_height = (unsigned)h;
    tp->pic.format.i_visible_width = (unsigned)w;
    tp->pic.format.i_visible_height = (unsigned)h;

    const int cw = up_chroma_dim(w, (int)sw);
    const int ch = up_chroma_dim(h, (int)sh);
    const int pw[3] = { w, cw, cw };
    const int ph[3] = { h, ch, ch };

    for (int k = 0; k < 3; k++) {
        int pitch = zt_align_up(pw[k]);
        size_t sz = (size_t)pitch * (size_t)ph[k];
        tp->buf[k] = aligned_alloc(ZT_ALIGN, sz);
        if (!tp->buf[k]) return -1;
        plane_t *p = &tp->pic.p[k];
        p->p_pixels        = tp->buf[k];
        p->i_pitch         = pitch;
        p->i_lines         = ph[k];
        p->i_pixel_pitch   = 1;
        p->i_visible_pitch = pw[k];
        p->i_visible_lines = ph[k];
    }
    return 0;
}

static inline void zt_pic_free(zt_pic_t *tp)
{
    for (int k = 0; k < 3; k++) { free(tp->buf[k]); tp->buf[k] = NULL; }
}

/* Deterministic xorshift fill of every plane's visible region. */
static inline void zt_pic_fill(zt_pic_t *tp, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x12345u;
    for (int k = 0; k < tp->pic.i_planes; k++) {
        plane_t *p = &tp->pic.p[k];
        for (int y = 0; y < p->i_visible_lines; y++) {
            uint8_t *row = p->p_pixels + (size_t)y * (size_t)p->i_pitch;
            for (int x = 0; x < p->i_visible_pitch; x++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                row[x] = (uint8_t)s;
            }
        }
    }
}

static inline void zt_pic_memset(zt_pic_t *tp, uint8_t v)
{
    for (int k = 0; k < tp->pic.i_planes; k++) {
        plane_t *p = &tp->pic.p[k];
        memset(p->p_pixels, v, (size_t)p->i_pitch * (size_t)p->i_lines);
    }
}

/* Fill a scaler_ctx_t for the zimg backend. log_obj NULL => no msg_* calls. */
static inline void zt_ctx_init(scaler_ctx_t *ctx, uint32_t chroma,
                               int sw, int sh, int dw, int dh,
                               int threads, int zerocopy)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->backend            = &scaler_backend_zimg_impl;
    ctx->src_w              = sw;
    ctx->src_h              = sh;
    ctx->src_coded_w        = (unsigned)sw;
    ctx->src_coded_h        = (unsigned)sh;
    ctx->dst_w              = dw;
    ctx->dst_h              = dh;
    ctx->algo               = UP_ALGO_LANCZOS;
    ctx->threads_pref       = threads;
    ctx->zimg.min_stripe_lines = 0;
    ctx->zimg.zerocopy      = zerocopy;
    /* Production default (REL-10): source-direct reads keep column tiling
     * eligible. Tests that need the copy-in / rows-only path override this
     * explicitly after init. */
    ctx->zimg.src_zerocopy  = 1;
    ctx->chroma             = chroma;
    ctx->log_obj            = NULL;
}

#endif /* ZIMG_TEST_UTIL_H */
