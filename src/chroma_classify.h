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

/* Match VLC's VLC_FOURCC byte ordering on both endianness paths. */
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
    /* This switch has CCN 7 by lizard's count (one branch per case).
     * That's still well below the 15 threshold and keeps the function
     * cheap to grep when adding new chromas. */
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

#endif /* AUTOUPSCALE_CHROMA_CLASSIFY_H */
