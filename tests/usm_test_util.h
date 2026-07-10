// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUTOUPSCALE_USM_TEST_UTIL_H
#define AUTOUPSCALE_USM_TEST_UTIL_H

#include "../src/usm.h"

#define UP_TEST_USM_APPLY_PLANE(dst_, dst_stride_, src_, src_stride_,       \
                                width_, height_, amount_q8_, workspace_)    \
    up_usm_apply_plane(&(up_usm_plane_io_t){                                \
        (dst_), (dst_stride_), (src_), (src_stride_), (width_), (height_)   \
    }, (amount_q8_), (workspace_))

#endif
