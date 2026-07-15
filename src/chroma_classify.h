// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * chroma_classify.h - shared software-chroma metadata and classification
 *****************************************************************************
 * up_chroma_descriptor(c) is the source of truth for every software format
 * the plugin accepts: pixel layout, plane count/order, subsampling, and
 * per-plane byte geometry. Picture views and both scaler
 * adapters derive their format handling from this table.
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
 *     exponents (0 = full rate, 1 = half rate) on each axis. Unlike the zimg
 *     adapter it covers semi-planar NV12/NV21 too: they are 4:2:0 whether or
 *     not zimg can consume them.
 *
 *   up_chroma_physical_plane_index(plane, swap_uv) - map semantic Y/U/V order
 *     to the physical plane order used by the VLC picture.
 *
 *   up_chroma_align_crop_even(c, w, h, x, y) - align a crop window inward to
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
#include <limits.h>
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

#define UP_CHROMA_MAX_PLANES 4

typedef enum {
    UP_CHROMA_LAYOUT_INVALID = 0,
    UP_CHROMA_LAYOUT_YUV420P,
    UP_CHROMA_LAYOUT_YUV422P,
    UP_CHROMA_LAYOUT_YUV444P,
    UP_CHROMA_LAYOUT_NV12,
    UP_CHROMA_LAYOUT_NV21,
    UP_CHROMA_LAYOUT_RGB24,
    UP_CHROMA_LAYOUT_RGBA,
    UP_CHROMA_LAYOUT_BGRA,
} up_chroma_layout_t;

typedef struct {
    uint32_t chroma;
    up_chroma_layout_t layout;
    uint8_t plane_count;
    uint8_t sub_w;
    uint8_t sub_h;
    bool uv_planes_swapped;
    uint8_t x_group_pixels[UP_CHROMA_MAX_PLANES];
    uint8_t x_group_bytes[UP_CHROMA_MAX_PLANES];
    uint8_t y_group_pixels[UP_CHROMA_MAX_PLANES];
    uint8_t pixel_pitch[UP_CHROMA_MAX_PLANES];
} up_chroma_descriptor_t;

static const up_chroma_descriptor_t up_chroma_descriptors[] = {
    { UP_FOURCC('I','4','2','0'), UP_CHROMA_LAYOUT_YUV420P,
      3, 1, 1, false,
      { 1, 2, 2, 0 }, { 1, 1, 1, 0 },
      { 1, 2, 2, 0 }, { 1, 1, 1, 0 } },
    { UP_FOURCC('Y','V','1','2'), UP_CHROMA_LAYOUT_YUV420P,
      3, 1, 1, true,
      { 1, 2, 2, 0 }, { 1, 1, 1, 0 },
      { 1, 2, 2, 0 }, { 1, 1, 1, 0 } },
    { UP_FOURCC('I','4','2','2'), UP_CHROMA_LAYOUT_YUV422P,
      3, 1, 0, false,
      { 1, 2, 2, 0 }, { 1, 1, 1, 0 },
      { 1, 1, 1, 0 }, { 1, 1, 1, 0 } },
    { UP_FOURCC('I','4','4','4'), UP_CHROMA_LAYOUT_YUV444P,
      3, 0, 0, false,
      { 1, 1, 1, 0 }, { 1, 1, 1, 0 },
      { 1, 1, 1, 0 }, { 1, 1, 1, 0 } },
    { UP_FOURCC('N','V','1','2'), UP_CHROMA_LAYOUT_NV12,
      2, 1, 1, false,
      { 1, 2, 0, 0 }, { 1, 2, 0, 0 },
      { 1, 2, 0, 0 }, { 1, 1, 0, 0 } },
    { UP_FOURCC('N','V','2','1'), UP_CHROMA_LAYOUT_NV21,
      2, 1, 1, false,
      { 1, 2, 0, 0 }, { 1, 2, 0, 0 },
      { 1, 2, 0, 0 }, { 1, 1, 0, 0 } },
    { UP_FOURCC('R','V','2','4'), UP_CHROMA_LAYOUT_RGB24,
      1, 0, 0, false,
      { 1, 0, 0, 0 }, { 3, 0, 0, 0 },
      { 1, 0, 0, 0 }, { 3, 0, 0, 0 } },
    { UP_FOURCC('R','G','B','A'), UP_CHROMA_LAYOUT_RGBA,
      1, 0, 0, false,
      { 1, 0, 0, 0 }, { 4, 0, 0, 0 },
      { 1, 0, 0, 0 }, { 4, 0, 0, 0 } },
    { UP_FOURCC('B','G','R','A'), UP_CHROMA_LAYOUT_BGRA,
      1, 0, 0, false,
      { 1, 0, 0, 0 }, { 4, 0, 0, 0 },
      { 1, 0, 0, 0 }, { 4, 0, 0, 0 } },
};

#define UP_CHROMA_DESCRIPTOR_COUNT \
    (sizeof up_chroma_descriptors / sizeof up_chroma_descriptors[0])

static inline const up_chroma_descriptor_t *
up_chroma_descriptor(uint32_t c)
{
    for (size_t i = 0; i < UP_CHROMA_DESCRIPTOR_COUNT; i++)
        if (up_chroma_descriptors[i].chroma == c)
            return &up_chroma_descriptors[i];
    return NULL;
}

static inline int up_chroma_physical_plane_index(int semantic_plane,
                                                 bool uv_planes_swapped)
{
    if (!uv_planes_swapped) return semantic_plane;
    if (semantic_plane == 1) return 2;
    if (semantic_plane == 2) return 1;
    return semantic_plane;
}

static inline bool up_chroma_layout_has_y(up_chroma_layout_t layout)
{
    switch (layout) {
        case UP_CHROMA_LAYOUT_YUV420P:
        case UP_CHROMA_LAYOUT_YUV422P:
        case UP_CHROMA_LAYOUT_YUV444P:
        case UP_CHROMA_LAYOUT_NV12:
        case UP_CHROMA_LAYOUT_NV21:
            return true;
        default:
            return false;
    }
}

static inline bool up_chroma_layout_is_planar(up_chroma_layout_t layout)
{
    switch (layout) {
        case UP_CHROMA_LAYOUT_YUV420P:
        case UP_CHROMA_LAYOUT_YUV422P:
        case UP_CHROMA_LAYOUT_YUV444P:
            return true;
        default:
            return false;
    }
}

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
    const up_chroma_descriptor_t *desc = up_chroma_descriptor(c);
    return desc != NULL && up_chroma_layout_has_y(desc->layout);
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
    const up_chroma_descriptor_t *desc = up_chroma_descriptor(c);
    if (desc == NULL || !up_chroma_layout_has_y(desc->layout))
        return false;
    *sub_w = desc->sub_w;
    *sub_h = desc->sub_h;
    return true;
}

/*
 * Align a source crop window inward to even on every subsampled axis.
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
 * A full axis call rounds the start up and the end down as one [start,end)
 * interval. Partial calls keep the older single-value behavior because the
 * production call sites still need to normalize dimensions or offsets alone.
 * Any of the four pointers may be NULL.
 */
static inline void up_chroma_align_axis_even(int *dim, unsigned *off)
{
    if (dim != NULL && off != NULL) {
        if (*dim <= 0) {
            *dim = 0;
            return;
        }
        const uint64_t start = *off;
        const uint64_t end = start + (uint64_t)*dim;
        const uint64_t aligned_start = (start + UINT64_C(1)) & ~UINT64_C(1);
        const uint64_t aligned_end = end & ~UINT64_C(1);
        *off = aligned_start <= UINT_MAX
             ? (unsigned)aligned_start : (UINT_MAX & ~1u);
        *dim = aligned_end > aligned_start
             ? (int)(aligned_end - aligned_start) : 0;
        return;
    }
    if (dim != NULL)
        *dim &= ~1;
    if (off != NULL)
        *off &= ~1u;
}

static inline void up_chroma_align_crop_even(uint32_t c, int *w, int *h,
                                             unsigned *x, unsigned *y)
{
    unsigned sub_w;
    unsigned sub_h;
    if (!up_chroma_subsample(c, &sub_w, &sub_h))
        return;
    if (sub_w)
        up_chroma_align_axis_even(w, x);
    if (sub_h)
        up_chroma_align_axis_even(h, y);
}

#endif /* AUTOUPSCALE_CHROMA_CLASSIFY_H */
