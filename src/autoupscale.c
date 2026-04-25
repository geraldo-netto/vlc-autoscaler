/*****************************************************************************
 * autoupscale.c — Real-time sub-720p video upscaler for VLC
 *****************************************************************************
 * Detects sub-720p video and upscales to 720p or 1080p using a pluggable
 * scaler backend (zimg preferred, swscale fallback). Optional luma USM
 * post-pass compensates for the resampler's slight softness.
 *
 * License: GPL-2.0-or-later (matches VLC core)
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#  include <sys/sysinfo.h>
#endif

#include "upscale_logic.h"
#include "usm.h"
#include "scaler.h"
#include "perfmon.h"
#include "threading.h"
#include "chroma_classify.h"

/* VLC's <libintl.h>-based N_() isn't always pulled in transitively.
 * Provide a no-op fallback if it's missing — we don't translate strings. */
#ifndef N_
#  define N_(s) (s)
#endif

/* True for chromas whose plane 0 is an 8-bit luma plane (Y).
 * USM is applied only to these — sharpening packed RGB or chroma planes
 * causes visible colour fringing on high-contrast edges. */
/* Chroma classification predicates live in chroma_classify.h so they
 * can be unit-tested without pulling in VLC. We keep these tiny VLC-typed
 * wrappers because the rest of the file uses vlc_fourcc_t. */

static bool ChromaIsOpaque( vlc_fourcc_t c )
{
    return up_chroma_is_opaque( (uint32_t)c );
}

static bool ChromaHasYPlane( vlc_fourcc_t c )
{
    return up_chroma_has_y_plane( (uint32_t)c );
}

/*****************************************************************************
 * Module options
 *****************************************************************************/
#define CFG_PREFIX "autoupscale-"

#define TARGET_TEXT     N_("Target resolution")
#define TARGET_LONGTEXT N_( \
    "0 = auto (decide between 720p and 1080p based on CPU/RAM), " \
    "1 = force 720p, 2 = force 1080p.")

#define ALGO_TEXT       N_("Scaling algorithm")
#define ALGO_LONGTEXT   N_( \
    "0 = fast bilinear (cheapest), " \
    "1 = bicubic (balanced), " \
    "2 = lanczos (default), " \
    "3 = spline36 (best for upscaling; zimg only — falls back to lanczos " \
    "on swscale).")

#define SKIP_TEXT       N_("Skip-above height")
#define SKIP_LONGTEXT   N_( \
    "Source heights >= this value are passed through untouched. " \
    "Default 720 — anything 720p or higher is left alone.")

#define USM_TEXT        N_("Unsharp-mask amount (post-upscale, percent)")
#define USM_LONGTEXT    N_( \
    "Amount of unsharp-mask sharpening applied to the luma plane after " \
    "the upscale, as a percentage. 0 disables. Default 30 (subtle). " \
    "Only applied to YUV chromas; ignored for RGB.")

#define BACKEND_TEXT    N_("Scaler backend")
#define BACKEND_LONGTEXT N_( \
    "0 = auto (zimg if compiled in, else swscale), " \
    "1 = force zimg (fail if unavailable), " \
    "2 = force swscale.")

#define TARGET_FPS_TEXT N_("Target frames per second for performance warning")
#define TARGET_FPS_LONGTEXT N_( \
    "If the average per-frame processing time exceeds 1/target_fps, the " \
    "plugin emits a one-time warning suggesting how to tune down. Set to " \
    "0 to disable performance monitoring. Default 60.")

#define THREADS_TEXT    N_("Number of worker threads for the scaler (planned, currently single-threaded)")
#define THREADS_LONGTEXT N_( \
    "Reserved for future slice-threaded zimg processing. Currently the " \
    "plugin runs single-threaded per frame regardless of this setting; " \
    "zimg's internal SIMD already provides significant within-thread " \
    "parallelism. The option is accepted now so configurations don't " \
    "break when slice threading lands. 0 = auto (cores - 2 in future), " \
    "1..64 = explicit count.")

/*****************************************************************************
 * Forward declarations
 *****************************************************************************/
static int        Open ( vlc_object_t * );
static void       Close( vlc_object_t * );
static picture_t *Filter( filter_t *, picture_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin()
    set_shortname( N_("AutoUpscale") )
    set_description( N_("Automatic sub-720p -> 720p/1080p video upscaler") )
    set_help( N_("Detects sub-720p video and upscales it to 720p or 1080p "
                 "in real time. Backends: zimg (preferred) and swscale.") )
    set_capability( "video filter", 0 )
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_callbacks( Open, Close )
    add_shortcut( "autoupscale" )

    add_integer_with_range( CFG_PREFIX "target", UP_TARGET_AUTO,
                            UP_TARGET_AUTO, UP_TARGET_1080P,
                            TARGET_TEXT, TARGET_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "algo", UP_ALGO_SPLINE36,
                            UP_ALGO_FAST_BILINEAR, UP_ALGO_MAX,
                            ALGO_TEXT, ALGO_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "skip-above", 720, 1, 8192,
                            SKIP_TEXT, SKIP_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "usm", UP_USM_AMOUNT_DEFAULT,
                            0, UP_USM_AMOUNT_MAX,
                            USM_TEXT, USM_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "backend", SCALER_BACKEND_AUTO,
                            SCALER_BACKEND_AUTO, SCALER_BACKEND_MAX,
                            BACKEND_TEXT, BACKEND_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "target-fps", 60, 0, 240,
                            TARGET_FPS_TEXT, TARGET_FPS_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "threads", UP_THREADS_AUTO,
                            0, UP_THREADS_MAX,
                            THREADS_TEXT, THREADS_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Internal state
 *****************************************************************************/
struct filter_sys_t
{
    scaler_ctx_t  scaler;

    /* Post-pass unsharp mask. Disabled when usm_amount_q8 == 0 OR
     * usm_workspace == NULL. */
    int           usm_amount_q8;
    uint8_t      *usm_workspace;

    /* Performance monitoring. Disabled when target_fps <= 0. */
    up_perfmon_t  perfmon;
    int           target_fps;     /* kept around so we can include it in warn msg */
    int           algo;           /* kept around for warn message */
    int           usm_pct;        /* kept around for warn message */
};

/*****************************************************************************
 * Helpers
 *****************************************************************************/

static void DetectHardware( long *cores, unsigned long *mem_mb )
{
    long c = sysconf( _SC_NPROCESSORS_ONLN );
    *cores = (c > 0) ? c : 1;
    *mem_mb = 0;
#ifdef __linux__
    struct sysinfo info;
    if( sysinfo( &info ) == 0 )
        *mem_mb = ((unsigned long)info.totalram * (unsigned long)info.mem_unit)
                  / (1024UL * 1024UL);
#endif
}

/*****************************************************************************
 * Open: probe input format, decide whether to engage, set up scaler + USM
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;

    const int src_w = p_filter->fmt_in.video.i_visible_width
                        ? p_filter->fmt_in.video.i_visible_width
                        : p_filter->fmt_in.video.i_width;
    const int src_h = p_filter->fmt_in.video.i_visible_height
                        ? p_filter->fmt_in.video.i_visible_height
                        : p_filter->fmt_in.video.i_height;

    const int skip_above = var_InheritInteger( p_filter,
                                               CFG_PREFIX "skip-above" );
    const int preset     = var_InheritInteger( p_filter,
                                               CFG_PREFIX "target" );
    const int algo       = var_InheritInteger( p_filter,
                                               CFG_PREFIX "algo" );
    const int backend_pref = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "backend" );

    long cores;
    unsigned long mem_mb;
    DetectHardware( &cores, &mem_mb );

    up_dims_t target = { 0, 0 };
    if( !up_plan_upscale( src_w, src_h, skip_above, preset,
                          cores, mem_mb, &target ) )
    {
        msg_Dbg( p_filter,
                 "AutoUpscale: bypassing %dx%d (skip>=%d, preset=%d)",
                 src_w, src_h, skip_above, preset );
        return VLC_EGENERIC;
    }

    /* Pick a scaler backend. */
    const vlc_fourcc_t chroma = p_filter->fmt_in.video.i_chroma;

    /* Reject hardware/opaque formats up front. We cannot read pixel data
     * from a VAAPI/VDPAU/D3D/MMAL/CVPX surface; VLC must insert a hw->sw
     * download converter before us. Failing here cleanly (instead of
     * accepting and then producing garbage) prompts VLC to do exactly
     * that, and it avoids the wasted scratch+thread allocation that
     * happens if we Open() and then get torn down on the next probe. */
    if( ChromaIsOpaque( chroma ) )
    {
        msg_Dbg( p_filter,
                 "AutoUpscale: declining opaque chroma 0x%08x; "
                 "VLC will insert a hw->sw converter and re-probe",
                 (unsigned)chroma );
        return VLC_EGENERIC;
    }

    const scaler_backend_t *be = scaler_pick( backend_pref, chroma, algo );
    if( !be )
    {
        msg_Warn( p_filter,
                  "AutoUpscale: no backend supports chroma 0x%08x with algo %d",
                  (unsigned)chroma, algo );
        return VLC_EGENERIC;
    }

    filter_sys_t *p_sys = calloc( 1, sizeof(*p_sys) );
    if( !p_sys ) return VLC_ENOMEM;

    p_sys->scaler.backend      = be;
    p_sys->scaler.src_w        = src_w;
    p_sys->scaler.src_h        = src_h;
    p_sys->scaler.dst_w        = target.width;
    p_sys->scaler.dst_h        = target.height;
    p_sys->scaler.algo         = algo;
    p_sys->scaler.threads_pref = var_InheritInteger( p_filter,
                                                     CFG_PREFIX "threads" );
    p_sys->scaler.chroma       = chroma;
    p_sys->scaler.log_obj      = p_this;

    if( be->open( &p_sys->scaler ) != 0 )
    {
        msg_Err( p_filter, "AutoUpscale: %s backend open failed", be->name );
        free( p_sys );
        return VLC_EGENERIC;
    }

    /* USM post-pass. */
    const int usm_pct = var_InheritInteger( p_filter, CFG_PREFIX "usm" );
    if( usm_pct > 0 && ChromaHasYPlane( chroma ) )
    {
        size_t ws_bytes = up_usm_workspace_size( target.width, target.height );
        if( ws_bytes > 0 )
        {
            p_sys->usm_workspace = malloc( ws_bytes );
            if( p_sys->usm_workspace )
                p_sys->usm_amount_q8 = up_usm_amount_pct_to_q8( usm_pct );
            else
                msg_Warn( p_filter,
                          "USM workspace alloc failed (%zu bytes); "
                          "sharpening disabled", ws_bytes );
        }
    }

    /* Performance monitor. */
    const int target_fps = var_InheritInteger( p_filter,
                                               CFG_PREFIX "target-fps" );
    up_perfmon_init( &p_sys->perfmon, target_fps );
    p_sys->target_fps = target_fps;
    p_sys->algo       = algo;
    p_sys->usm_pct    = usm_pct;

    /* Output format: same chroma, new dimensions. */
    p_filter->fmt_out.video.i_chroma         = chroma;
    p_filter->fmt_out.video.i_width          = target.width;
    p_filter->fmt_out.video.i_visible_width  = target.width;
    p_filter->fmt_out.video.i_height         = target.height;
    p_filter->fmt_out.video.i_visible_height = target.height;
    p_filter->fmt_out.video.i_x_offset       = 0;
    p_filter->fmt_out.video.i_y_offset       = 0;

    p_filter->p_sys           = p_sys;
    p_filter->pf_video_filter = Filter;

    /* Resolve the user's thread preference for the engagement log. */
    int threads_resolved = up_threads_decide(
        p_sys->scaler.threads_pref, cores );

    msg_Info( p_filter,
              "AutoUpscale engaged: %dx%d -> %dx%d "
              "(backend=%s preset=%d algo=%d usm=%d fps_target=%d "
              "threads=%d cores=%ld mem=%luMB)",
              src_w, src_h, target.width, target.height,
              be->name, preset, algo, usm_pct, target_fps, threads_resolved,
              cores, mem_mb );

    return VLC_SUCCESS;
}

/* Read CLOCK_MONOTONIC in nanoseconds. Falls back to 0 on failure (perfmon
 * ignores non-positive samples). */
static inline int64_t monotonic_ns(void)
{
    struct timespec ts;
    if( clock_gettime( CLOCK_MONOTONIC, &ts ) != 0 ) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/*****************************************************************************
 * Filter: scale one picture, then USM, then performance accounting
 *****************************************************************************/
static picture_t *Filter( filter_t *p_filter, picture_t *p_in )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    if( !p_in ) return NULL;

    picture_t *p_out = filter_NewPicture( p_filter );
    if( !p_out )
    {
        picture_Release( p_in );
        return NULL;
    }

    int64_t t_start = monotonic_ns();

    if( p_sys->scaler.backend->process( &p_sys->scaler, p_in, p_out ) != 0 )
    {
        picture_Release( p_out );
        picture_Release( p_in );
        return NULL;
    }

    if( p_sys->usm_amount_q8 > 0 && p_sys->usm_workspace
        && p_out->i_planes >= 1 )
    {
        plane_t *y = &p_out->p[0];
        up_usm_apply_plane(
            y->p_pixels, y->i_pitch,
            y->p_pixels, y->i_pitch,        /* in-place */
            p_sys->scaler.dst_w, p_sys->scaler.dst_h,
            p_sys->usm_amount_q8,
            p_sys->usm_workspace );
    }

    int64_t t_end = monotonic_ns();
    int64_t elapsed_ns = (t_start > 0 && t_end > t_start) ? t_end - t_start : 0;

    if( up_perfmon_record_ns( &p_sys->perfmon, elapsed_ns ) )
    {
        /* One-time perf hint with concrete tuning suggestions.
         * Emitted as msg_Info, not msg_Warn, because VLC 3.x's default
         * verbosity suppresses level-2 warnings — we want this visible
         * without users having to pass --verbose=1. The "Performance
         * warning:" prefix in the body makes the intent unambiguous. */
        long ewma_us   = (long)up_perfmon_ewma_us( &p_sys->perfmon );
        long budget_us = (long)up_perfmon_budget_us( &p_sys->perfmon );
        msg_Info( p_filter,
                  "Performance warning: avg frame work is %ld us, "
                  "exceeding the %d FPS budget of %ld us "
                  "(backend=%s algo=%d usm=%d).",
                  ewma_us, p_sys->target_fps, budget_us,
                  p_sys->scaler.backend->name,
                  p_sys->algo, p_sys->usm_pct );
        msg_Info( p_filter,
                  "  To tune down, try (in order of decreasing quality cost):" );
        if( p_sys->algo == UP_ALGO_SPLINE36 )
            msg_Info( p_filter,
                      "    --autoupscale-algo=2   "
                      "(lanczos: similar quality, faster)" );
        if( p_sys->algo > UP_ALGO_BICUBIC )
            msg_Info( p_filter,
                      "    --autoupscale-algo=1   "
                      "(bicubic: noticeably faster, slight quality drop)" );
        if( p_sys->usm_pct > 0 )
            msg_Info( p_filter,
                      "    --autoupscale-usm=0    "
                      "(disable post-sharpening)" );
        if( p_sys->scaler.backend->name[0] != 's' )  /* not already swscale */
            msg_Info( p_filter,
                      "    --autoupscale-backend=2 "
                      "(force swscale: faster scaler)" );
        msg_Info( p_filter,
                  "    --autoupscale-target=1 "
                  "(force 720p instead of 1080p: 2.25x less work)" );
        msg_Info( p_filter,
                  "  Or set --autoupscale-target-fps=0 to silence this warning." );
    }

    picture_CopyProperties( p_out, p_in );
    picture_Release( p_in );
    return p_out;
}

/*****************************************************************************
 * Close: tear down
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    if( p_sys )
    {
        if( p_sys->scaler.backend )
            p_sys->scaler.backend->close( &p_sys->scaler );
        free( p_sys->usm_workspace );
        free( p_sys );
    }
}
