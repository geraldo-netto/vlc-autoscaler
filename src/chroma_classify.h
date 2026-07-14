// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * chroma_classify.h - pure chroma-fourcc classification helpers
 *****************************************************************************
 * Two small predicates for chroma fourccs:
 *
 *   up_chroma_is_opaque(c) - true if c is a hardware/opaque GPU surface
 *     format (VAAPI, VDPAU, D3D9/11, MMAL, CoreVideo). The autoupscale
 *     filter cannot read pixels from these and must fail Open() so VLC
 *     inserts a hw->sw download converter and re-probes us with the
 *     resolved software chroma. Accepting opaque chromas wastes thread
 *     spawns during VLC's filter-chain probing and may surface as
 *     "Too high level of recursion" if the chain solver runs out of
 *     depth threading converters around a hardware-decoded source.
 *
 *   up_chroma_has_y_plane(c) - true if c is a planar/semi-planar YUV
 *     format with a discrete luma plane that USM can sharpen. Packed
 *     YUV (YUY2/UYVY) and RGB are excluded; sharpening packed RGB or
 *     chroma planes causes visible colour fringing on edges.
 *
 *   up_chroma_subsample(c, &sub_w, &sub_h) - the chroma subsample shift
 *     exponents (0 = full rate, 1 = half rate) on each axis. This is the
 *     single source of truth for subsampling: scaler_zimg_chroma.h derives
 *     its (sub_w, sub_h) from it, and picture_view.h's per-plane group
 *     table must agree with it (tests/test_chroma_classify.c asserts that).
 *     Unlike the zimg mapping it covers the semi-planar NV12/NV21 too -
 *     they are 4:2:0 whether or not zimg can consume them.
 *
 *   up_chroma_align_crop_even(c, w, h, x, y) - round a crop window down to
 *     even on each subsampled axis. Any NULL argument is skipped.
 *
 * The fourcc literals are spelled out as 4-character UP_FOURCC() calls
 * so this header doesn't pull in any VLC headers - the test files can
 * include it directly. The values are byte-identical to VLC's
 * VLC_CODEC_* constants on both little- and big-endian hosts.
 *****************************************************************************/

#ifndef AUTOUPSCALE_CHROMA_CLASSIFY_H
#define AUTOUPSCALE_CHROMA_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Match VLC's VLC_FOURCC byte ordering on both endianness paths.
 * WORDS_BIGENDIAN comes from VLC's config.h, which the plugin build
 * does not define — fall back to the compiler's __BYTE_ORDER__ so a
 * big-endian host picks the right branch without it. The
 * _Static_asserts in autoupscale.c pin these against VLC_CODEC_*, so
 * any residual mismatch is a compile break, not silent corruption. */
#if !defined(WORDS_BIGENDIAN) && defined(__BYTE_ORDER__) && \
    defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
# define WORDS_BIGENDIAN 1
#endif
#ifdef WORDS_BIGENDIAN
# define UP_FOURCC( a, b, c, d ) \
    ( ((uint32_t)(d)) | ( ((uint32_t)(c)) << 8 ) \
      | ( ((uint32_t)(b)) << 16 ) | ( ((uint32_t)(a)) << 24 ) )
#else
# define UP_FOURCC( a, b, c, d ) \
    ( ((uint32_t)(a)) | ( ((uint32_t)(b)) << 8 ) \
      | ( ((uint32_t)(c)) << 16 ) | ( ((uint32_t)(d)) << 24 ) )
#endif

/*
 * Hardware/opaque chroma fourccs. Update when VLC adds a new hwaccel
 * surface format. Keep alphabetical-ish by family for readability.
 */
static const uint32_t up_opaque_chromas[] = {
    UP_FOURCC('V','A','O','P'),  /* VAAPI 4:2:0 8bpc */
    UP_FOURCC('V','A','O','0'),  /* VAAPI 4:2:0 10bpc */
    UP_FOURCC('V','D','V','0'),  /* VDPAU 4:2:0 */
    UP_FOURCC('V','D','V','2'),  /* VDPAU 4:2:2 */
    UP_FOURCC('V','D','V','4'),  /* VDPAU 4:4:4 */
    UP_FOURCC('V','D','O','R'),  /* VDPAU output */
    UP_FOURCC('D','X','A','9'),  /* D3D9 8bpc */
    UP_FOURCC('D','X','A','0'),  /* D3D9 10bpc */
    UP_FOURCC('D','X','1','1'),  /* D3D11 8bpc */
    UP_FOURCC('D','X','1','0'),  /* D3D11 10bpc */
    UP_FOURCC('M','M','A','L'),  /* MMAL (Raspberry Pi) */
    UP_FOURCC('C','V','P','N'),  /* CoreVideo NV12 */
    UP_FOURCC('C','V','P','Y'),  /* CoreVideo UYVY */
    UP_FOURCC('C','V','P','I'),  /* CoreVideo I420 */
    UP_FOURCC('C','V','P','B'),  /* CoreVideo BGRA */
    UP_FOURCC('C','V','P','P'),  /* CoreVideo P010 */
};
#define UP_OPAQUE_CHROMA_COUNT \
    (sizeof up_opaque_chromas / sizeof up_opaque_chromas[0])

static inline bool up_chroma_is_opaque(uint32_t c)
{
    for (size_t i = 0; i < UP_OPAQUE_CHROMA_COUNT; i++)
        if (c == up_opaque_chromas[i])
            return true;
    return false;
}

/*
 * Chromas with a discrete luma (Y) plane that USM can sharpen. Packed
 * YUV (YUY2/UYVY) deliberately omitted - depacking just to apply a
 * 3x3 convolution would burn more cycles than the sharpen saves.
 */
static inline bool up_chroma_has_y_plane(uint32_t c)
{
    /* A flat switch keeps new chroma mappings easy to audit. */
    switch (c) {
        case UP_FOURCC('I','4','2','0'):  /* planar 4:2:0 */
        case UP_FOURCC('Y','V','1','2'):  /* planar 4:2:0, V/U swap */
        case UP_FOURCC('N','V','1','2'):  /* semi-planar 4:2:0 */
        case UP_FOURCC('N','V','2','1'):  /* semi-planar 4:2:0, V/U swap */
        case UP_FOURCC('I','4','2','2'):  /* planar 4:2:2 */
        case UP_FOURCC('I','4','4','4'):  /* planar 4:4:4 */
            return true;
        default:
            return false;
    }
}

/*
 * Chroma subsample shift exponents, per axis. Returns false (and leaves the
 * outputs untouched) for chromas with no discrete Y plane - packed YUV, RGB
 * and the opaque surfaces, none of which this filter reads plane-wise.
 */
static inline bool up_chroma_subsample(uint32_t c,
                                       unsigned *sub_w, unsigned *sub_h)
{
    if (sub_w == NULL || sub_h == NULL)
        return false;
    /* A flat switch keeps new chroma mappings easy to audit. */
    switch (c) {
        case UP_FOURCC('I','4','2','0'):  /* planar 4:2:0 */
        case UP_FOURCC('Y','V','1','2'):  /* planar 4:2:0, V/U swap */
        case UP_FOURCC('N','V','1','2'):  /* semi-planar 4:2:0 */
        case UP_FOURCC('N','V','2','1'):  /* semi-planar 4:2:0, V/U swap */
            *sub_w = 1; *sub_h = 1; return true;
        case UP_FOURCC('I','4','2','2'):  /* planar 4:2:2 */
            *sub_w = 1; *sub_h = 0; return true;
        case UP_FOURCC('I','4','4','4'):  /* planar 4:4:4 */
            *sub_w = 0; *sub_h = 0; return true;
        default:
            return false;
    }
}

/*
 * Round a source crop window down to even on every subsampled axis.
 *
 * A subsampled chroma plane anchors its crop at offset >> subsample, floored
 * (up_picture_plane_extent). An odd luma offset therefore floors the chroma
 * anchor and shifts chroma half a luma pel against luma - visible colour
 * fringing on saturated edges, identical on both backends because they share
 * picture_view, so the byte-identity tests cannot see it. Odd crop *dims* are
 * just as bad on the zimg path: the graph build rejects an image dimension
 * that is not divisible by the subsample factor, which turns into a sticky
 * lazy-init failure and every frame dropped. VP9/AV1 permit both with 4:2:0.
 *
 * Rounding down only ever shrinks the window, so offset+width stays inside
 * the coded plane. Any of the four pointers may be NULL.
 */
static inline void up_chroma_align_crop_even(uint32_t c, int *w, int *h,
                                             unsigned *x, unsigned *y)
{
    unsigned sub_w;
    unsigned sub_h;
    if (!up_chroma_subsample(c, &sub_w, &sub_h))
        return;
    if (sub_w) {
        if (w) *w &= ~1;
        if (x) *x &= ~1u;
    }
    if (sub_h) {
        if (h) *h &= ~1;
        if (y) *y &= ~1u;
    }
}

#endif /* AUTOUPSCALE_CHROMA_CLASSIFY_H */
