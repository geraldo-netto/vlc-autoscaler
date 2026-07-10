// SPDX-License-Identifier: GPL-2.0-or-later
/*****************************************************************************
 * scaler_pick_logic.h - pure backend-dispatch logic
 *****************************************************************************
 * Routes a (preference, chroma, algo) request to one of two backends.
 * The full scaler_backend_t struct (defined in scaler.h) drags in VLC
 * picture_t through its open/process/close function pointers, which
 * makes unit testing without VLC awkward.
 *
 * This header peels off just the dispatch logic into a duck-typed
 * helper that operates on void pointers and an explicit "supports"
 * callback. Both production (scaler.c) and unit tests
 * (test_scaler_pick.c) use the same code path, but tests can supply
 * mock backends without pulling in VLC headers.
 *
 * The function returns one of the two input pointers (zimg or swscale)
 * or NULL. It never dereferences anything other than the supports
 * callbacks, so the void-pointer-as-handle pattern is safe.
 *****************************************************************************/

#ifndef AUTOUPSCALE_SCALER_PICK_LOGIC_H
#define AUTOUPSCALE_SCALER_PICK_LOGIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Duplicates the constants from scaler.h so callers don't need that
 * header (which pulls in <vlc_common.h>). KEEP IN SYNC. */
#ifndef SCALER_PICK_AUTO
#define SCALER_PICK_AUTO     0
#define SCALER_PICK_ZIMG     1
#define SCALER_PICK_SWSCALE  2
#endif

typedef int (*up_supports_fn)(uint32_t chroma, int algo);
typedef int (*up_scaler_open_fn)(void *context, const void *backend_handle);

/*
 * Try the preferred backend and, when explicitly allowed, one distinct
 * fallback. The callback follows scaler_backend_t::open's convention:
 * zero means success and any non-zero value means failure. Context is an
 * opaque value passed through unchanged and may be NULL.
 *
 * Returns the handle whose open callback succeeded, or NULL. A missing
 * preferred handle or callback fails without invoking anything. A fallback
 * is never retried when it aliases the preferred handle.
 */
static inline const void *up_scaler_open_with_fallback(
    const void *preferred_handle, const void *fallback_handle,
    void *context, up_scaler_open_fn open_backend, bool allow_fallback)
{
    if (preferred_handle == NULL || open_backend == NULL)
        return NULL;
    if (open_backend(context, preferred_handle) == 0)
        return preferred_handle;
    if (!allow_fallback || fallback_handle == NULL ||
        fallback_handle == preferred_handle)
        return NULL;
    if (open_backend(context, fallback_handle) == 0)
        return fallback_handle;
    return NULL;
}

static inline const void *up_pick_zimg_if_ok(
    const void *zimg_handle, up_supports_fn zimg_supports,
    uint32_t chroma, int algo)
{
    if (zimg_handle && zimg_supports && zimg_supports(chroma, algo))
        return zimg_handle;
    return NULL;
}

static inline const void *up_pick_swscale_if_ok(
    const void *swscale_handle, up_supports_fn swscale_supports,
    uint32_t chroma, int algo)
{
    if (swscale_supports(chroma, algo))
        return swscale_handle;
    return NULL;
}

static inline const void *up_pick_auto(
    const void *zimg_handle, up_supports_fn zimg_supports,
    const void *swscale_handle, up_supports_fn swscale_supports,
    uint32_t chroma, int algo)
{
    /* Try zimg first (better quality, supports Spline36).
     * Fall through to swscale on any miss. */
    const void *z = up_pick_zimg_if_ok(zimg_handle, zimg_supports,
                                       chroma, algo);
    if (z)
        return z;
    return up_pick_swscale_if_ok(swscale_handle, swscale_supports,
                                 chroma, algo);
}

/*
 * Pure dispatch. zimg_handle may be NULL (e.g. when libzimg wasn't
 * linked). swscale_handle must not be NULL. zimg_supports and
 * swscale_supports may be NULL when the corresponding handle is NULL.
 *
 * Returns zimg_handle, swscale_handle, or NULL.
 */
static inline const void *up_scaler_pick_with(
    const void *zimg_handle, up_supports_fn zimg_supports,
    const void *swscale_handle, up_supports_fn swscale_supports,
    int pref, uint32_t chroma, int algo)
{
    if (swscale_handle == NULL || swscale_supports == NULL)
        return NULL;

    switch (pref) {
        case SCALER_PICK_ZIMG:
            return up_pick_zimg_if_ok(zimg_handle, zimg_supports,
                                      chroma, algo);
        case SCALER_PICK_SWSCALE:
            return up_pick_swscale_if_ok(swscale_handle, swscale_supports,
                                         chroma, algo);
        case SCALER_PICK_AUTO:
        default:
            return up_pick_auto(zimg_handle, zimg_supports,
                                swscale_handle, swscale_supports,
                                chroma, algo);
    }
}

#endif /* AUTOUPSCALE_SCALER_PICK_LOGIC_H */
