/*****************************************************************************
 * scaler.c — backend selection
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "scaler.h"

extern const scaler_backend_t scaler_backend_swscale_impl;
#ifdef HAVE_ZIMG
extern const scaler_backend_t scaler_backend_zimg_impl;
#endif

const scaler_backend_t *scaler_pick( int pref,
                                     vlc_fourcc_t chroma, int algo )
{
    const scaler_backend_t *zimg    = NULL;
    const scaler_backend_t *swscale = &scaler_backend_swscale_impl;

#ifdef HAVE_ZIMG
    zimg = &scaler_backend_zimg_impl;
#endif

    switch( pref )
    {
        case SCALER_BACKEND_ZIMG:
            if( zimg && zimg->supports( chroma, algo ) ) return zimg;
            return NULL;

        case SCALER_BACKEND_SWSCALE:
            if( swscale->supports( chroma, algo ) ) return swscale;
            return NULL;

        case SCALER_BACKEND_AUTO:
        default:
            /* Try zimg first (better quality, supports Spline36).
             * Fall through to swscale on any miss. */
            if( zimg && zimg->supports( chroma, algo ) ) return zimg;
            if( swscale->supports( chroma, algo ) ) return swscale;
            return NULL;
    }
}
