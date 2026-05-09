// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "usm_pool.h"
#include "scaler.h"
#include "perfmon.h"
#include "threading.h"
#include "chroma_classify.h"
#include "content_probe.h"

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
    "1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 4K (2160p), " \
    "5 = 5K (2880p), 6 = 8K (4320p). " \
    "Targets above 1080p require explicit selection. " \
    "All targets are subject to the 4x linear ratio cap relative to source.")

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

#define THREADS_TEXT    N_("Number of worker threads for the zimg scaler")
#define THREADS_LONGTEXT N_( \
    "0 = auto (cores/2 - 2, clamped to [1,64]); 1..64 = explicit count. " \
    "The zimg backend partitions each frame into N horizontal stripes " \
    "and runs one persistent worker thread per stripe. swscale backend " \
    "runs single-threaded regardless. Higher values reduce per-frame " \
    "latency at the cost of more memory and lower per-thread cache " \
    "locality; the auto default reserves half the machine for the rest " \
    "of VLC and other libraries. Override if you measured otherwise.")

#define ZEROCOPY_DST_TEXT N_("Write directly to VLC's destination picture")
#define ZEROCOPY_DST_LONGTEXT N_( \
    "1 = on (default): worker threads write directly into VLC's " \
    "destination picture, skipping a final memcpy. Saves about 125 " \
    "microseconds per 1080p frame (~0.8% of a 60 fps budget) and ~3 " \
    "MB of scratch memory. 0 = off (safe fallback): worker threads " \
    "write to plugin-owned scratch buffers, then a final memcpy moves " \
    "the result into VLC's destination picture. Set to 0 if you see " \
    "garbled output, crashes, or other instability with the default - " \
    "the writeback to VLC's destination picture has been verified " \
    "byte-identical to the copy-out path in our testing but cannot be " \
    "fully verified across every VLC build configuration.")

#define USM_STRIPE_MIN_ROWS_TEXT N_("Minimum rows per USM stripe")
#define USM_STRIPE_MIN_ROWS_LONGTEXT N_( \
    "Each USM worker thread processes at least this many rows. " \
    "Smaller values let more workers fit on low-resolution frames " \
    "(e.g. 480p) but increase per-frame thread-dispatch overhead. " \
    "Default 8 (matches the kernel boundary handling). " \
    "Range 1..256.")

#define ZIMG_STRIPE_LINES_TEXT N_("Minimum dst lines per zimg stripe")
#define ZIMG_STRIPE_LINES_LONGTEXT N_( \
    "Each zimg worker thread emits at least this many destination " \
    "rows. Smaller values let more workers fit on low-res output but " \
    "increase per-stripe boundary work. Default 16. Range 4..128.")

#define USM_SHARP_THRESH_TEXT N_("USM-skip sharpness threshold")
#define USM_SHARP_THRESH_LONGTEXT N_( \
    "Mean Laplacian-variance value above which the source is " \
    "considered heavily textured/grainy and the USM post-pass is " \
    "skipped for the rest of playback (USM on grainy content " \
    "amplifies noise without adding perceived sharpness). " \
    "Lower = trip more aggressively (skip USM on more sources). " \
    "Higher = trip rarely (USM stays on most content). " \
    "0 = feature off, USM always runs regardless of source. " \
    "Default 3500. Range 0..20000.")

#define PROBE_TEXT N_("Content-aware quality probe")
#define PROBE_LONGTEXT N_( \
    "1 = on (default): observe the first ~60 frames of luma to " \
    "estimate source quality. If the source is both very soft (low " \
    "Laplacian variance: heavy blur or noise reduction) AND very " \
    "blocky (high edge intensity at 8-pixel boundaries: heavy " \
    "compression), log a one-time advisory recommending the user " \
    "disable AutoUpscale for this source. The probe is DIAGNOSTIC " \
    "only — VLC 3's filter API does not allow runtime format " \
    "renegotiation, so the filter cannot self-bypass mid-stream. " \
    "Probe cost is ~50us/frame at 480p (only during the first 60 " \
    "frames). 0 = off: skip the probe entirely. The probe is also " \
    "skipped automatically for non-planar chromas (no Y plane).")

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
                            UP_TARGET_AUTO, UP_TARGET_MAX,
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
    add_integer_with_range( CFG_PREFIX "zerocopy-dst", 1, 0, 1,
                            ZEROCOPY_DST_TEXT, ZEROCOPY_DST_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "content-probe", 1, 0, 1,
                            PROBE_TEXT, PROBE_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "usm-stripe-min-rows", 0, 0, 256,
                            USM_STRIPE_MIN_ROWS_TEXT,
                            USM_STRIPE_MIN_ROWS_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "zimg-stripe-lines", 0, 0, 128,
                            ZIMG_STRIPE_LINES_TEXT,
                            ZIMG_STRIPE_LINES_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "usm-sharp-threshold",
                            UP_PROBE_THRESH_SHARP_LAP_MEAN,
                            0, 20000,
                            USM_SHARP_THRESH_TEXT,
                            USM_SHARP_THRESH_LONGTEXT, false )
vlc_module_end()

/*****************************************************************************
 * Internal state
 *****************************************************************************/
struct filter_sys_t
{
    scaler_ctx_t  scaler;

    /* Post-pass unsharp mask. Disabled when usm_amount_q8 == 0 OR
     * usm_pool == NULL. The pool encapsulates the workspace and the
     * worker threads (lazy-spawned on first apply); when amount is 0,
     * apply() takes a fast identity path with no thread activity. */
    int           usm_amount_q8;
    usm_pool_t   *usm_pool;

    /* Performance monitoring. Disabled when target_fps <= 0. */
    up_perfmon_t  perfmon;
    int           target_fps;     /* kept around so we can include it in warn msg */
    int           algo;           /* kept around for warn message */
    int           usm_pct;        /* kept around for warn message */

    /* Content-aware bypass. The probe runs over the first
     * UP_PROBE_WINDOW_FRAMES frames, accumulating Laplacian variance
     * and block-edge intensity on the luma plane. Once the window
     * closes, up_should_bypass_for_content() decides whether the source
     * is so soft+blocky that upscaling actively hurts.
     *
     * IMPORTANT scope note: VLC 3's video-filter API doesn't allow
     * runtime format renegotiation, so we can't actually "stop scaling"
     * mid-stream — the downstream chain expects target-resolution
     * pictures from frame 1. Instead, when the probe decides the
     * upscale is hurting, we log a one-time msg_Info recommending the
     * user disable the filter (or set --autoupscale-target=1) for next
     * playback, and we keep scaling. This is HONEST about what's
     * achievable; a real bypass would need a structural change to
     * VLC's filter graph that we can't make from a video filter.
     *
     *   probe_enabled : 1 if the probe is configured to run (option), 0 disabled
     *   probe_active  : 1 while we're still collecting samples
     *   advice_logged : 1 once we've logged the bypass recommendation
     *   probe_accum   : accumulator state passed to up_probe_observe()
     */
    int                probe_enabled;
    int                probe_active;
    int                advice_logged;
    up_probe_accum_t   probe_accum;

    /* Set after probe completes when source is heavily textured/grainy.
     * Causes ApplyUsmIfEnabled to return early — USM on such content
     * mostly amplifies noise. See up_should_skip_usm_for_sharpness. */
    int                usm_skip_sharp;

    /* User tunables read at Open(); see corresponding option longtexts.
     * usm_sharp_threshold == 0 disables the feature (USM always runs);
     * any positive value is the lap_mean cutoff above which USM is
     * skipped after the probe completes. */
    int                usm_sharp_threshold;
};

/* How many frames to observe before deciding. At 30fps this is 2 seconds
 * — enough for a few I-frames and a couple of GOPs to characterize the
 * encoder's quality across motion changes. */
#define UP_PROBE_WINDOW_FRAMES 60

/*****************************************************************************
 * Helpers
 *****************************************************************************/

static void DetectHardware( int *cores, unsigned long *mem_mb )
{
    *cores = up_detect_cores();
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
 *****************************************************************************
 * Open() is split into focused phase helpers (each at CCN <= 5):
 *   ResolveInputDims      — picks visible-or-physical src dims
 *   PickBackendOrReject   — opaque-chroma + null-backend gate
 *   ConfigureScaler       — fills scaler_ctx_t from VLC vars
 *   InitUsmPool           — optional USM post-pass setup
 *   InitProbeAndPerfmon   — perfmon + content-probe bookkeeping
 *   SetOutputFormat       — fmt_out wiring
 * Open() itself is a linear orchestrator at CCN ~5.
 *****************************************************************************/

/* Pick the visible (cropped) dimension when present, otherwise the
 * physical one. Encapsulates the two ?: that previously inflated Open's
 * branch count. */
static void ResolveInputDims( const filter_t *p_filter,
                              int *src_w, int *src_h )
{
    *src_w = p_filter->fmt_in.video.i_visible_width
                ? p_filter->fmt_in.video.i_visible_width
                : p_filter->fmt_in.video.i_width;
    *src_h = p_filter->fmt_in.video.i_visible_height
                ? p_filter->fmt_in.video.i_visible_height
                : p_filter->fmt_in.video.i_height;
}

/* Opaque-chroma rejection + backend selection. Logs the reason and returns
 * NULL when this filter cannot run on the input — Open() turns NULL into
 * VLC_EGENERIC. CCN 4. */
static const scaler_backend_t *PickBackendOrReject(
    filter_t *p_filter, vlc_fourcc_t chroma, int algo, int backend_pref )
{
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
        return NULL;
    }

    const scaler_backend_t *be = scaler_pick( backend_pref, chroma, algo );
    if( !be )
    {
        msg_Warn( p_filter,
                  "AutoUpscale: no backend supports chroma 0x%08x with algo %d",
                  (unsigned)chroma, algo );
        return NULL;
    }
    return be;
}

/* Populate the scaler_ctx_t from filter parameters and VLC vars. CCN 2. */
static void ConfigureScaler( scaler_ctx_t *sc,
                             const scaler_backend_t *be,
                             filter_t *p_filter, vlc_object_t *p_this,
                             vlc_fourcc_t chroma, int algo,
                             int src_w, int src_h, up_dims_t target )
{
    sc->backend      = be;
    sc->src_w        = src_w;
    sc->src_h        = src_h;
    sc->dst_w        = target.width;
    sc->dst_h        = target.height;
    sc->algo         = algo;
    sc->threads_pref = var_InheritInteger( p_filter, CFG_PREFIX "threads" );
    sc->zimg_stripe_min_lines = var_InheritInteger( p_filter,
        CFG_PREFIX "zimg-stripe-lines" );
    sc->dst_zerocopy = var_InheritInteger( p_filter, CFG_PREFIX "zerocopy-dst" );
    sc->chroma       = chroma;
    sc->log_obj      = p_this;

    if( !sc->dst_zerocopy )
        msg_Info( p_filter,
                  "AutoUpscale: dst zero-copy DISABLED via "
                  "--autoupscale-zerocopy-dst=0 (using copy-out path)" );
}

/* Lazily create the USM pool when the user asked for sharpening AND the
 * chroma has a Y plane. On allocation failure we log and continue with
 * USM disabled — the upscale itself doesn't depend on it. CCN 4. */
static void InitUsmPool( filter_sys_t *p_sys, filter_t *p_filter,
                         vlc_fourcc_t chroma, up_dims_t target,
                         int cores, int usm_pct )
{
    if( usm_pct <= 0 || !ChromaHasYPlane( chroma ) )
        return;

    int n_threads = up_threads_decide( p_sys->scaler.threads_pref, cores );
    int stripe_min = var_InheritInteger( p_filter,
                                         CFG_PREFIX "usm-stripe-min-rows" );
    p_sys->usm_pool = up_usm_pool_create( n_threads,
                                          target.width, target.height,
                                          stripe_min );
    if( p_sys->usm_pool )
        p_sys->usm_amount_q8 = up_usm_amount_pct_to_q8( usm_pct );
    else
        msg_Warn( p_filter,
                  "USM pool create failed (%dx%d, %d threads); "
                  "sharpening disabled",
                  target.width, target.height, n_threads );
}

/* Initialize the perfmon and the content probe bookkeeping fields. CCN 1. */
static void InitProbeAndPerfmon( filter_sys_t *p_sys, filter_t *p_filter,
                                 vlc_fourcc_t chroma, int algo, int usm_pct )
{
    const int target_fps = var_InheritInteger( p_filter,
                                               CFG_PREFIX "target-fps" );
    up_perfmon_init( &p_sys->perfmon, target_fps );
    p_sys->target_fps = target_fps;
    p_sys->algo       = algo;
    p_sys->usm_pct    = usm_pct;

    /* Content-aware bypass probe. Runs only when the source has a
     * planar Y component (probe metrics are computed on luma only).
     * For opaque/packed/RGB chromas the probe stays disabled — those
     * are either GPU-managed (can't read) or have no luma plane.
     * The accumulator is already zeroed by calloc(). */
    p_sys->probe_enabled = var_InheritInteger( p_filter,
                                               CFG_PREFIX "content-probe" )
                       && ChromaHasYPlane( chroma );
    p_sys->probe_active  = p_sys->probe_enabled;
    p_sys->advice_logged = 0;

    p_sys->usm_sharp_threshold = var_InheritInteger( p_filter,
        CFG_PREFIX "usm-sharp-threshold" );
}

/* Wire fmt_out to the upscale target. Same chroma, new dimensions. CCN 1. */
static void SetOutputFormat( filter_t *p_filter, vlc_fourcc_t chroma,
                             up_dims_t target )
{
    p_filter->fmt_out.video.i_chroma         = chroma;
    p_filter->fmt_out.video.i_width          = target.width;
    p_filter->fmt_out.video.i_visible_width  = target.width;
    p_filter->fmt_out.video.i_height         = target.height;
    p_filter->fmt_out.video.i_visible_height = target.height;
    p_filter->fmt_out.video.i_x_offset       = 0;
    p_filter->fmt_out.video.i_y_offset       = 0;
}

static int Open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;

    int src_w, src_h;
    ResolveInputDims( p_filter, &src_w, &src_h );

    const int skip_above   = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "skip-above" );
    const int preset       = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "target" );
    const int algo         = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "algo" );
    const int backend_pref = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "backend" );
    const int usm_pct      = var_InheritInteger( p_filter,
                                                 CFG_PREFIX "usm" );

    int cores;
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

    const vlc_fourcc_t chroma = p_filter->fmt_in.video.i_chroma;
    const scaler_backend_t *be = PickBackendOrReject( p_filter, chroma,
                                                     algo, backend_pref );
    if( !be )
        return VLC_EGENERIC;

    filter_sys_t *p_sys = calloc( 1, sizeof(*p_sys) );
    if( !p_sys ) return VLC_ENOMEM;

    ConfigureScaler( &p_sys->scaler, be, p_filter, p_this,
                     chroma, algo, src_w, src_h, target );

    if( be->open( &p_sys->scaler ) != 0 )
    {
        msg_Err( p_filter, "AutoUpscale: %s backend open failed", be->name );
        free( p_sys );
        return VLC_EGENERIC;
    }

    InitUsmPool( p_sys, p_filter, chroma, target, cores, usm_pct );
    InitProbeAndPerfmon( p_sys, p_filter, chroma, algo, usm_pct );
    SetOutputFormat( p_filter, chroma, target );

    p_filter->p_sys           = p_sys;
    p_filter->pf_video_filter = Filter;

    int threads_resolved = up_threads_decide(
        p_sys->scaler.threads_pref, cores );

    msg_Info( p_filter,
              "AutoUpscale engaged: %dx%d -> %dx%d "
              "(backend=%s preset=%d algo=%d usm=%d fps_target=%d "
              "threads=%d cores=%d mem=%luMB simd=%s)",
              src_w, src_h, target.width, target.height,
              be->name, preset, algo, usm_pct, p_sys->target_fps,
              threads_resolved, cores, mem_mb, up_usm_pool_variant_name );

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
/*
 * Emit the one-time performance-tuning advisory, with concrete
 * suggestions tailored to the current configuration. Triggered when
 * up_perfmon_record_ns() returns 1 (meaning EWMA exceeded the budget
 * for the first time after warmup). Only fires once per filter
 * lifetime — the perfmon's internal state ensures that.
 *
 * Emitted as msg_Info (not msg_Warn) because VLC 3.x's default
 * verbosity suppresses level-2 warnings; we want the advisory visible
 * without users having to pass --verbose=1.
 */
static void EmitPerfAdvisory( filter_t *p_filter, filter_sys_t *p_sys )
{
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

/*
 * Run one iteration of the content probe on the source frame, and
 * close the probe (logging an advisory) once the window is full.
 * Observe-only — does not modify p_in or any output. Called from
 * Filter() while p_sys->probe_active is true and p_in has at least
 * one plane (planar chromas only; the probe_enabled gate in Open()
 * already filtered out opaque/packed sources).
 *
 * Extracted from Filter() to keep its cyclomatic complexity under
 * the project's CCN-15 ceiling.
 */
static void RunProbe( filter_t *p_filter, filter_sys_t *p_sys,
                      const picture_t *p_in )
{
    const plane_t *y = &p_in->p[0];
    int w = y->i_visible_pitch ? y->i_visible_pitch : y->i_pitch;
    int h = y->i_visible_lines ? y->i_visible_lines : y->i_lines;
    uint64_t lap_n = 0, edge_n = 0;
    uint64_t lap  = up_laplacian_variance(  y->p_pixels, y->i_pitch,
                                            w, h, &lap_n );
    uint64_t edge = up_block_edge_strength( y->p_pixels, y->i_pitch,
                                            w, h, &edge_n );
    up_probe_observe( &p_sys->probe_accum, lap, lap_n, edge, edge_n );

    if( p_sys->probe_accum.frames < UP_PROBE_WINDOW_FRAMES )
        return;

    p_sys->probe_active = 0;
    if( p_sys->usm_sharp_threshold > 0
        && p_sys->probe_accum.lap_samples > 0
        && (p_sys->probe_accum.lap_sum / p_sys->probe_accum.lap_samples)
           > (uint64_t)p_sys->usm_sharp_threshold )
    {
        p_sys->usm_skip_sharp = 1;
        msg_Info( p_filter,
                  "AutoUpscale: source is heavily textured "
                  "(lap_mean=%llu > threshold=%d); skipping post-USM "
                  "to avoid amplifying grain. "
                  "Override via --autoupscale-usm-sharp-threshold=0 "
                  "(disable) or a different cutoff.",
                  (unsigned long long)(p_sys->probe_accum.lap_sum
                       / p_sys->probe_accum.lap_samples),
                  p_sys->usm_sharp_threshold );
    }
    int recommend_bypass = up_should_bypass_for_content( &p_sys->probe_accum );
    uint64_t lap_mean  = p_sys->probe_accum.lap_samples
        ? p_sys->probe_accum.lap_sum  / p_sys->probe_accum.lap_samples : 0;
    uint64_t edge_mean = p_sys->probe_accum.edge_samples
        ? p_sys->probe_accum.edge_sum / p_sys->probe_accum.edge_samples : 0;

    if( recommend_bypass && !p_sys->advice_logged )
    {
        p_sys->advice_logged = 1;
        msg_Info( p_filter,
                  "AutoUpscale: content probe complete: source is "
                  "soft (lap_mean=%llu) AND blocky (edge_mean=%llu). "
                  "Upscaling is amplifying compression artifacts "
                  "without recovering detail.",
                  (unsigned long long)lap_mean,
                  (unsigned long long)edge_mean );
        msg_Info( p_filter,
                  "  Consider disabling AutoUpscale for this source, "
                  "or set --autoupscale-target=1 to halve the per-"
                  "frame cost. Set --autoupscale-content-probe=0 to "
                  "silence this message." );
    }
    else
    {
        msg_Dbg( p_filter,
                 "AutoUpscale: content probe complete (lap_mean=%llu "
                 "edge_mean=%llu): upscale is appropriate.",
                 (unsigned long long)lap_mean,
                 (unsigned long long)edge_mean );
    }
}

/* Apply the post-pass USM in-place on the luma plane, when enabled.
 * No-op when amount==0, no pool allocated, or no luma plane. CCN 2. */
static void ApplyUsmIfEnabled( filter_sys_t *p_sys, picture_t *p_out )
{
    if( p_sys->usm_amount_q8 <= 0 || !p_sys->usm_pool
        || p_out->i_planes < 1 || p_sys->usm_skip_sharp )
        return;

    plane_t *y = &p_out->p[0];
    up_usm_pool_apply(
        p_sys->usm_pool,
        y->p_pixels, y->i_pitch,
        y->p_pixels, y->i_pitch,        /* in-place */
        p_sys->usm_amount_q8 );
}

/* Record one frame's elapsed work into the perfmon and emit the one-time
 * advisory if perfmon decides the budget has been blown. CCN 2. */
static void RecordPerf( filter_t *p_filter, filter_sys_t *p_sys,
                        int64_t t_start, int64_t t_end )
{
    int64_t elapsed_ns = (t_start > 0 && t_end > t_start) ? t_end - t_start : 0;
    if( up_perfmon_record_ns( &p_sys->perfmon, elapsed_ns ) )
        EmitPerfAdvisory( p_filter, p_sys );
}

static picture_t *Filter( filter_t *p_filter, picture_t *p_in )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    if( !p_in ) return NULL;

    if( p_sys->probe_active && p_in->i_planes >= 1 )
        RunProbe( p_filter, p_sys, p_in );

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

    ApplyUsmIfEnabled( p_sys, p_out );
    RecordPerf( p_filter, p_sys, t_start, monotonic_ns() );

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
        up_usm_pool_destroy( p_sys->usm_pool );
        free( p_sys );
    }
}
