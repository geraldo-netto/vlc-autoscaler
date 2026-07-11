// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * abi_assert.c — pin VLC struct layout assumptions to the REAL headers (ABI-1)
 *****************************************************************************
 * The unit/coverage tests compile production sources (scaler_swscale.c,
 * picture_view.h) against the hand-written layouts in tests/stubs/. Nothing
 * otherwise stops those stubs from drifting from the real VLC 3.0 structs: an
 * upstream rename or retype of a field the code reads would keep the stub-built
 * tests green while the shipped .so — built against the real headers — breaks.
 *
 * This TU is compiled ONLY against the real VLC headers (no tests/stubs on the
 * include path) and static-asserts the name and exact type of every field the
 * production code touches. It emits no code; a mismatch is a build failure.
 * Keep the asserted set in sync with what tests/stubs/ mirrors.
 *****************************************************************************/

#include <vlc_common.h>
#include <vlc_picture.h>
#include <vlc_fourcc.h>

#include <stddef.h>
#include <stdint.h>

/* Field exists with exactly `type`. The _Generic controlling expression is
 * unevaluated, so the null-pointer deref never happens. */
#define ABI_FIELD_TYPE(agg, field, type)                                      \
    _Static_assert(_Generic(((agg *)0)->field, type: 1, default: 0),          \
                   #agg "." #field " is not " #type " (VLC layout drift?)")

/* plane_t — read by picture_view.h and scaler_swscale.c. */
ABI_FIELD_TYPE(plane_t, p_pixels,       uint8_t *);
ABI_FIELD_TYPE(plane_t, i_lines,        int);
ABI_FIELD_TYPE(plane_t, i_pitch,        int);
ABI_FIELD_TYPE(plane_t, i_pixel_pitch,  int);
ABI_FIELD_TYPE(plane_t, i_visible_lines, int);
ABI_FIELD_TYPE(plane_t, i_visible_pitch, int);

/* video_format_t — read via filter_t.fmt_in.video and picture_t.format. */
ABI_FIELD_TYPE(video_format_t, i_chroma,         vlc_fourcc_t);
ABI_FIELD_TYPE(video_format_t, i_width,          unsigned int);
ABI_FIELD_TYPE(video_format_t, i_height,         unsigned int);
ABI_FIELD_TYPE(video_format_t, i_visible_width,  unsigned int);
ABI_FIELD_TYPE(video_format_t, i_visible_height, unsigned int);
ABI_FIELD_TYPE(video_format_t, i_x_offset,       unsigned int);
ABI_FIELD_TYPE(video_format_t, i_y_offset,       unsigned int);

/* picture_t — the fields the backends walk. `format` is video_frame_format_t,
 * a typedef of video_format_t; `p[]` holds plane_t. */
ABI_FIELD_TYPE(picture_t, i_planes, int);
_Static_assert(sizeof(((picture_t *)0)->format) == sizeof(video_format_t),
               "picture_t.format is not video_format_t (VLC layout drift?)");
_Static_assert(sizeof(((picture_t *)0)->p[0]) == sizeof(plane_t),
               "picture_t.p[] element is not plane_t (VLC layout drift?)");
_Static_assert(sizeof(((picture_t *)0)->p) / sizeof(plane_t) >= 4,
               "picture_t.p[] holds fewer planes than the backends index");
