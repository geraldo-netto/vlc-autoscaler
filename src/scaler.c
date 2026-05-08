// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler.c — backend selection
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"
#include "scaler_pick_logic.h"

extern const scaler_backend_t scaler_backend_swscale_impl;
#ifdef HAVE_ZIMG
extern const scaler_backend_t scaler_backend_zimg_impl;
#endif

/* Verify SCALER_BACKEND_* values match SCALER_PICK_* — we duplicate
 * them in scaler_pick_logic.h to avoid pulling VLC headers into tests.
 */
_Static_assert(SCALER_BACKEND_AUTO    == SCALER_PICK_AUTO,    "AUTO mismatch");
_Static_assert(SCALER_BACKEND_ZIMG    == SCALER_PICK_ZIMG,    "ZIMG mismatch");
_Static_assert(SCALER_BACKEND_SWSCALE == SCALER_PICK_SWSCALE, "SWSCALE mismatch");

const scaler_backend_t *scaler_pick( int pref,
                                     vlc_fourcc_t chroma, int algo )
{
    const scaler_backend_t *zimg    = NULL;
    const scaler_backend_t *swscale = &scaler_backend_swscale_impl;

#ifdef HAVE_ZIMG
    zimg = &scaler_backend_zimg_impl;
#endif

    /* swscale is unconditionally linked, so its supports() is always live.
     * zimg is conditionally NULL when HAVE_ZIMG is undefined; cppcheck sees
     * only one config and flags the ternary as always-true OR always-false
     * depending on which it analyses — both are spurious. */
    return (const scaler_backend_t *)up_scaler_pick_with(
        zimg,
        // cppcheck-suppress knownConditionTrueFalse
        zimg ? zimg->supports : NULL,
        swscale, swscale->supports,
        pref, (uint32_t)chroma, algo);
}
