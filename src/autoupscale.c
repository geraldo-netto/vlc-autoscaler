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

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#  include <sys/sysinfo.h>
#endif

#include "cpu_level.h"
#include "upscale_logic.h"
#include "usm.h"
#include "usm_pool.h"
#include "scaler.h"
#include "scaler_pick_logic.h"
#include "perfmon.h"
#include "threading.h"
#include "chroma_classify.h"
#include "content_probe.h"
#include "picture_view.h"

/* ARCH-2: chroma_classify.h spells its chroma fourccs as UP_FOURCC() literals
 * so the tests can include it without VLC headers. Guard against drift from
 * VLC's real VLC_CODEC_* values with compile-time asserts in this (VLC-linked)
 * TU. Check every software descriptor entry; the opaque/hwaccel fourccs have
 * no stable VLC_CODEC_* names across VLC versions. */
_Static_assert(UP_FOURCC('I','4','2','0') == VLC_CODEC_I420, "I420 fourcc drift");
_Static_assert(UP_FOURCC('Y','V','1','2') == VLC_CODEC_YV12, "YV12 fourcc drift");
_Static_assert(UP_FOURCC('N','V','1','2') == VLC_CODEC_NV12, "NV12 fourcc drift");
_Static_assert(UP_FOURCC('N','V','2','1') == VLC_CODEC_NV21, "NV21 fourcc drift");
_Static_assert(UP_FOURCC('I','4','2','2') == VLC_CODEC_I422, "I422 fourcc drift");
_Static_assert(UP_FOURCC('I','4','4','4') == VLC_CODEC_I444, "I444 fourcc drift");
_Static_assert(UP_FOURCC('R','V','2','4') == VLC_CODEC_RGB24, "RGB24 fourcc drift");
_Static_assert(UP_FOURCC('R','G','B','A') == VLC_CODEC_RGBA, "RGBA fourcc drift");
_Static_assert(UP_FOURCC('B','G','R','A') == VLC_CODEC_BGRA, "BGRA fourcc drift");

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
    "2 = lanczos, " \
    "3 = spline36 (default; best for upscaling; zimg only — falls back to lanczos " \
    "on swscale).")

#define SKIP_TEXT       N_("Skip-above height")
#define SKIP_LONGTEXT   N_( \
    "Source heights >= this value are passed through untouched. " \
    "Default 720 — anything 720p or higher is left alone. " \
    "Only honoured when --autoupscale-target=0 (AUTO); explicit " \
    "presets (1..6) bypass the skip-above gate and remain subject " \
    "to the 4x upscale cap. Value 0 disables the gate " \
    "(AUTO engages on any sub-target source).")

#define USM_TEXT        N_("Unsharp-mask amount (post-upscale, percent)")
#define USM_LONGTEXT    N_( \
    "Amount of unsharp-mask sharpening applied to the luma plane after " \
    "the upscale, as a percentage. 0 disables. Default 20 (subtle). " \
    "Only applied to YUV chromas; ignored for RGB.")

#define BACKEND_TEXT    N_("Scaler backend")
#define BACKEND_LONGTEXT N_( \
    "0 = auto: prefer zimg, use swscale on support/open failure, and " \
    "switch once after a fatal zimg processing failure. Transient frame " \
    "failures do not switch. 1 = strict zimg (fail if unavailable), " \
    "2 = strict swscale.")

#define TARGET_FPS_TEXT N_("Target frames per second for performance warning")
#define TARGET_FPS_LONGTEXT N_( \
    "If the average per-frame processing time exceeds 1/target_fps, the " \
    "plugin emits a one-time warning suggesting how to tune down. Set to " \
    "0 to disable only that advisory; EWMA telemetry remains active. " \
    "Default 60.")

#define THREADS_TEXT    N_("Worker preference for zimg and USM")
#define THREADS_LONGTEXT N_( \
    "Worker preference shared by zimg and USM. 0 = auto (cores/2 - 2, " \
    "clamped to [1,64]); 1..64 = explicit preference, capped by CPUs " \
    "allowed to the process. Each pool may clamp lower for frame geometry. " \
    "The zimg backend normally uses horizontal stripes and adds column " \
    "tiles for very wide/short frames. It runs one persistent worker " \
    "per grid cell; swscale remains single-threaded. Higher values can " \
    "reduce per-frame " \
    "latency at the cost of more memory and lower per-thread cache " \
    "locality; the auto default reserves half the machine for the rest " \
    "of VLC and other libraries. Override if you measured otherwise.")

#define PIN_TEXT        N_("Pin scaler worker threads to CPU cores")
#define PIN_LONGTEXT    N_( \
    "0 = off (default; let the OS scheduler place threads). 1 = pin each " \
    "zimg scaler worker thread to a distinct CPU core (round-robin). " \
    "Linux only; best-effort (ignored if it fails). Off by default because " \
    "pinning can HURT on a typical desktop by fighting VLC's other threads " \
    "and the scheduler's load balancing — enable only on a dedicated, " \
    "high-core-count or NUMA transcode box where you measured a gain. Does " \
    "not affect the USM sharpening pool.")

#define ZEROCOPY_DST_TEXT N_("Write directly to VLC's destination picture")
#define ZEROCOPY_DST_LONGTEXT N_( \
    "1 = on (default): on aligned row-only grids, workers write directly " \
    "into VLC's destination picture, skipping copy-out. Column grids " \
    "always use per-tile scratch. This avoids a frame-sized copy and its " \
    "persistent plane scratch. 0 = off (safe fallback): worker threads " \
    "write to plugin-owned scratch buffers and copy their output regions " \
    "into VLC's destination picture. Set to 0 if you see " \
    "garbled output, crashes, or other instability with the default - " \
    "the writeback to VLC's destination picture has been verified " \
    "byte-identical to the copy-out path in our testing but cannot be " \
    "fully verified across every VLC build configuration.")

#define ZEROCOPY_SRC_TEXT N_("Read VLC's source picture directly")
#define ZEROCOPY_SRC_LONGTEXT N_( \
    "1 = on (default): zimg worker threads read VLC's source picture " \
    "directly, skipping copy-in and its persistent plane scratch. The " \
    "symmetric twin of zerocopy-dst (which writes VLC's " \
    "destination picture directly). 0 = off (safe fallback): the source is " \
    "copied to plugin-owned scratch first, then the worker graphs read the " \
    "scratch. On very wide/short frames setting 0 also disables column " \
    "tiling (fewer workers), since tiles need direct source reads. " \
    "Set to 0 if you see garbled output, crashes, or instability - " \
    "reading VLC's pool-managed source buffers from worker threads has been " \
    "verified byte-identical to the copy-in path in our harness but, like " \
    "zerocopy-dst, cannot be fully verified across every VLC build " \
    "configuration.")

#define USM_STRIPE_MIN_ROWS_TEXT N_("Minimum rows per USM stripe")
#define USM_STRIPE_MIN_ROWS_LONGTEXT N_( \
    "Each USM worker thread processes at least this many rows. " \
    "Smaller values let more workers fit on low-resolution frames " \
    "(e.g. 480p) but increase per-frame thread-dispatch overhead. " \
    "0 = auto (8, matches the kernel boundary handling). " \
    "Range 0..256.")

#define ZIMG_STRIPE_LINES_TEXT N_("Minimum dst lines per zimg stripe")
#define ZIMG_STRIPE_LINES_LONGTEXT N_( \
    "Each zimg worker thread emits at least this many destination " \
    "rows. Smaller values let more workers fit on low-res output but " \
    "increase per-stripe boundary work. 0 = auto (16). Range 0..128.")

#define USM_SHARP_THRESH_TEXT N_("USM-skip sharpness threshold")
#define USM_SHARP_THRESH_LONGTEXT N_( \
    "Mean squared Laplacian-response value above which the source is " \
    "considered heavily textured/grainy and the USM post-pass is " \
    "skipped for the rest of playback (USM on grainy content " \
    "amplifies noise without adding perceived sharpness). " \
    "Lower = trip more aggressively (skip USM on more sources). " \
    "Higher = trip rarely (USM stays on most content). " \
    "0 = feature off, USM always runs regardless of source. " \
    "Independent of --autoupscale-content-probe: the sharpness metric " \
    "is collected even with the probe advisory disabled. " \
    "Default 3500. Range 0..20000.")

#define PROBE_TEXT N_("Content-aware quality probe")
#define PROBE_LONGTEXT N_( \
    "1 = on (default): observe a fixed initial luma window to " \
    "estimate source quality. If the source is both very soft (low " \
    "mean squared Laplacian response: heavy blur or noise reduction) " \
    "AND very " \
    "blocky (high edge intensity at 8-pixel boundaries: heavy " \
    "compression), log a one-time advisory recommending the user " \
    "disable AutoUpscale for this source. The probe is DIAGNOSTIC " \
    "only — VLC 3's filter API does not allow runtime format " \
    "renegotiation, so the filter cannot self-bypass mid-stream. " \
    "The fixed sampling step spans the visible plane, so cost scales " \
    "with its dimensions and stops after the probe window. 0 = off: " \
    "no advisory is logged (metric collection still " \
    "runs when --autoupscale-usm-sharp-threshold needs it, since that " \
    "gate changes pixel output and is not diagnostic). The probe is " \
    "skipped automatically for chromas without a readable Y plane.")

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
    add_integer_with_range( CFG_PREFIX "skip-above", 720, 0, 8192,
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
    add_integer_with_range( CFG_PREFIX "pin-threads", 0, 0, 1,
                            PIN_TEXT, PIN_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "zerocopy-dst", 1, 0, 1,
                            ZEROCOPY_DST_TEXT, ZEROCOPY_DST_LONGTEXT, false )
    add_integer_with_range( CFG_PREFIX "zerocopy-src", 1, 0, 1,
                            ZEROCOPY_SRC_TEXT, ZEROCOPY_SRC_LONGTEXT, false )
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
typedef struct source_crop_s
{
    up_dims_t dims;
    unsigned x_offset;
    unsigned y_offset;
} source_crop_t;

struct filter_sys_t
{
    scaler_ctx_t  scaler;

    /* Post-pass unsharp mask. A zero amount or NULL pool makes the plugin skip
     * USM. A nonzero pool lazily owns its workers and private row scratch. */
    int           usm_amount_q8;
    usm_pool_t   *usm_pool;

    /* Performance telemetry. target_fps <= 0 disables only the advisory. */
    up_perfmon_t  perfmon;
    int           target_fps;     /* kept around so we can include it in warn msg */
    int           algo;           /* kept around for warn message */
    int           usm_pct;        /* kept around for warn message */

    /* Content-aware advisory. The probe runs over the first
     * UP_PROBE_WINDOW_FRAMES valid views, accumulating squared Laplacian energy
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
     *   enabled       : 1 if metric collection runs — either the
     *                   content-probe option is on, or the USM sharpness
     *                   gate needs the metrics (usm > 0 and
     *                   usm-sharp-threshold > 0)
     *   advice        : 1 if the bypass advisory may be logged
     *                   (content-probe option only)
     *   active        : 1 while we're still collecting samples
     *   advice_logged : 1 once we've logged the bypass recommendation
     *   accum         : accumulator state passed to up_probe_observe()
     */
    struct
    {
        int              enabled;
        int              advice;
        int              active;
        int              advice_logged;
        up_probe_accum_t accum;
    } probe;

    /* Set after probe completes when source is heavily textured/grainy.
     * Causes ApplyUsmIfEnabled to return early — USM on such content
     * mostly amplifies noise. See up_should_skip_usm_for_sharpness. */
    int                usm_skip_sharp;

    /* User tunables read at Open(); see corresponding option longtexts.
     * usm_sharp_threshold == 0 disables the feature (USM always runs);
     * any positive value is the lap_mean cutoff above which USM is
     * skipped after the probe completes. */
    int                usm_sharp_threshold;

    /* One-shot guard so a persistently failing backend logs once instead of
     * spamming the log every frame (OBS-1). */
    int                process_fail_logged;

    /* One-shot guard for output-pool exhaustion (OBS-2): filter_NewPicture
     * returning NULL drops the frame; latch so backpressure logs once, not
     * once per dropped frame. */
    int                newpic_fail_logged;

    /* SYS-2 runtime fallback state. backend_pref is the user's
     * --autoupscale-backend so a forced zimg is respected; fallback_tried
     * makes the swap one-shot. A failed swap leaves scaler.backend NULL
     * (backend dead — Filter drops without touching the closed priv). */
    int                backend_pref;
    int                fallback_tried;

    uint64_t           frame_count;

    /* OBS-3: periodic long-run visibility (frames processed/dropped + EWMA). */
    uint64_t           dropped_count;
    int64_t            last_stats_ns;

    /* ERR-1: both OBS-5 stat variables were created; gates the periodic
     * var_SetInteger export and the paired var_Destroy at Close(). */
    int                stats_vars_ok;

    /* One-shot log of the USM pool's real worker count, deferred to after
     * the first apply() because lazy init may shrink it (the engagement
     * log's threads= reflects neither pool). */
    int                usm_workers_logged;
};

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
    {
        /* Compute in 64-bit: on 32-bit hosts `totalram * mem_unit` overflows
         * `unsigned long` and would yield a bogus (small) mem_mb that skews
         * the AUTO 720p/1080p decision. Saturate when narrowing back. */
        uint64_t mb = ( (uint64_t)info.totalram * (uint64_t)info.mem_unit )
                      >> 20;  /* / (1024*1024) */
        *mem_mb = ( mb > (uint64_t)ULONG_MAX ) ? ULONG_MAX
                                               : (unsigned long)mb;
    }
#endif
}

/*****************************************************************************
 * Open: probe input format, decide whether to engage, set up scaler + USM
 *****************************************************************************
 * Open() is split into focused phase helpers:
 *   ResolveSourceCrop     — picks and aligns visible-or-physical source crop
 *   PickBackendOrReject   — opaque-chroma + null-backend gate
 *   ConfigureScaler       — fills scaler_ctx_t from VLC vars
 *   InitUsmPool           — optional USM post-pass setup
 *   InitProbeAndPerfmon   — perfmon + content-probe bookkeeping
 *   SetOutputFormat       — fmt_out wiring
 * Open() itself is a linear orchestrator.
 *****************************************************************************/

/* Pick the visible (cropped) dimension when present, otherwise the physical
 * one, then align origin and extent together for subsampled chromas. */
static source_crop_t ResolveSourceCrop( const filter_t *p_filter )
{
    const unsigned width = p_filter->fmt_in.video.i_visible_width
                ? p_filter->fmt_in.video.i_visible_width
                : p_filter->fmt_in.video.i_width;
    const unsigned height = p_filter->fmt_in.video.i_visible_height
                ? p_filter->fmt_in.video.i_visible_height
                : p_filter->fmt_in.video.i_height;
    source_crop_t crop = {
        .dims = {
            width <= INT_MAX ? (int)width : -1,
            height <= INT_MAX ? (int)height : -1,
        },
        .x_offset = p_filter->fmt_in.video.i_x_offset,
        .y_offset = p_filter->fmt_in.video.i_y_offset,
    };
    up_chroma_align_crop_even( p_filter->fmt_in.video.i_chroma,
                               &crop.dims.width, &crop.dims.height,
                               &crop.x_offset, &crop.y_offset );
    return crop;
}

/* Opaque-chroma rejection + backend selection. Logs the reason and returns
 * NULL when this filter cannot run on the input — Open() turns NULL into
 * VLC_EGENERIC. */
static const scaler_backend_t *PickBackendOrReject(
    filter_t *p_filter, vlc_fourcc_t chroma, int algo, int backend_pref )
{
    /* Reject hardware/opaque formats up front. We cannot read pixel data
     * from a VAAPI/VDPAU surface; VLC must insert a hw->sw
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

static int OpenBackendAttempt( void *context, const void *backend_handle )
{
    scaler_ctx_t *sc = context;
    const scaler_backend_t *be = backend_handle;
    sc->backend = be;
    return be->open( sc );
}

/* AUTO may recover from a preferred zimg open failure through the broad-coverage
 * swscale backend. Explicit backend selections remain strict. */
static int OpenScalerOrFallback( filter_t *p_filter, filter_sys_t *p_sys )
{
    scaler_ctx_t *sc = &p_sys->scaler;
    const scaler_backend_t *preferred = sc->backend;
    const bool allow_fallback = p_sys->backend_pref == SCALER_BACKEND_AUTO
                             && preferred->id == SCALER_BACKEND_ZIMG;
    const scaler_backend_t *fallback = allow_fallback
        ? scaler_pick( SCALER_BACKEND_SWSCALE, sc->chroma, sc->algo )
        : NULL;
    const scaler_backend_t *selected = up_scaler_open_with_fallback(
        preferred, fallback, sc, OpenBackendAttempt, allow_fallback );

    if( selected == preferred )
        return 0;
    if( fallback && selected == fallback )
    {
        msg_Warn( p_filter,
                  "AutoUpscale: %s backend open failed; using %s fallback",
                  preferred->name, fallback->name );
        return 0;
    }
    if( allow_fallback && fallback )
        msg_Err( p_filter,
                 "AutoUpscale: %s and %s fallback backend open failed",
                 preferred->name, fallback->name );
    else
        msg_Err( p_filter, "AutoUpscale: %s backend open failed",
                 preferred->name );
    return -1;
}

/* var_InheritInteger returns int64_t while every AutoUpscale tunable is an
 * int. VLC range-clamps declared options, but narrow explicitly and
 * saturate so an out-of-range value can never truncate (sonar 64->32). */
static int InheritIntSat( filter_t *p_filter, const char *name )
{
    int64_t v = var_InheritInteger( p_filter, name );
    if( v > INT_MAX ) return INT_MAX;
    if( v < INT_MIN ) return INT_MIN;
    return (int)v;
}

/* Populate the scaler_ctx_t from filter parameters and VLC vars. */
static void ConfigureScaler( scaler_ctx_t *sc,
                             const scaler_backend_t *be,
                             filter_t *p_filter,
                             vlc_fourcc_t chroma, int algo,
                             source_crop_t src, up_dims_t target )
{
    vlc_object_t *p_this = (vlc_object_t *)p_filter;
    int threads = InheritIntSat( p_filter, CFG_PREFIX "threads" );
    if( threads < 0 ) threads = 0;
    if( threads > UP_THREADS_MAX ) threads = UP_THREADS_MAX;

    sc->backend      = be;
    sc->src_w        = src.dims.width;
    sc->src_h        = src.dims.height;
    sc->src_coded_w  = p_filter->fmt_in.video.i_width;
    sc->src_coded_h  = p_filter->fmt_in.video.i_height;
    sc->src_x_offset = src.x_offset;
    sc->src_y_offset = src.y_offset;
    sc->dst_w        = target.width;
    sc->dst_h        = target.height;
    sc->algo         = algo;
    sc->threads_pref = threads;
    sc->pin_cpus     = InheritIntSat( p_filter,
        CFG_PREFIX "pin-threads" ) ? 1 : 0;
    sc->zimg.min_stripe_lines = InheritIntSat( p_filter,
        CFG_PREFIX "zimg-stripe-lines" );
    sc->zimg.zerocopy = InheritIntSat( p_filter, CFG_PREFIX "zerocopy-dst" );
    sc->zimg.src_zerocopy = InheritIntSat( p_filter,
        CFG_PREFIX "zerocopy-src" );
    sc->chroma       = chroma;
    sc->log_obj      = p_this;

    if( !sc->zimg.zerocopy )
        msg_Info( p_filter,
                  "AutoUpscale: dst zero-copy DISABLED via "
                  "--autoupscale-zerocopy-dst=0 (using copy-out path)" );
    if( !sc->zimg.src_zerocopy )
        msg_Info( p_filter,
                  "AutoUpscale: src zero-copy DISABLED via "
                  "--autoupscale-zerocopy-src=0 (using copy-in path, "
                  "rows-only tiling)" );
}

/* Create the small USM descriptor when sharpening is requested and the chroma
 * has a Y plane. Workers and scratch initialize lazily on first apply. On
 * allocation failure, continue with USM disabled. */
static void InitUsmPool( filter_sys_t *p_sys, filter_t *p_filter,
                         vlc_fourcc_t chroma, up_dims_t target,
                         int cores, int usm_pct )
{
    if( usm_pct <= 0 || !ChromaHasYPlane( chroma ) )
        return;

    int n_threads = up_threads_decide( p_sys->scaler.threads_pref, cores );
    int stripe_min = InheritIntSat( p_filter,
                                         CFG_PREFIX "usm-stripe-min-rows" );
    p_sys->usm_pool = up_usm_pool_create( n_threads,
                                          target.width, target.height,
                                          stripe_min );
    if( p_sys->usm_pool )
        p_sys->usm_amount_q8 = up_usm_amount_pct_to_q8( usm_pct );
    else
        msg_Info( p_filter,
                  "USM pool create failed (%dx%d, %d threads); "
                  "sharpening disabled",
                  target.width, target.height, n_threads );
}

/* Initialize the perfmon and the content probe bookkeeping fields. */
static void InitProbeAndPerfmon( filter_sys_t *p_sys, filter_t *p_filter,
                                 vlc_fourcc_t chroma, int algo, int usm_pct )
{
    const int target_fps = InheritIntSat( p_filter,
                                               CFG_PREFIX "target-fps" );
    up_perfmon_init( &p_sys->perfmon, target_fps );
    p_sys->target_fps = target_fps;
    p_sys->algo       = algo;
    p_sys->usm_pct    = usm_pct;

    /* Content-aware metric probe. Runs only when the source exposes a
     * readable luma plane. For opaque and packed RGB chromas the probe stays
     * disabled because they are GPU-managed or have no discrete luma plane.
     * The accumulator is already zeroed by calloc(). */
    p_sys->usm_sharp_threshold = InheritIntSat( p_filter,
        CFG_PREFIX "usm-sharp-threshold" );

    /* Metric collection also runs with content-probe=0 when the USM
     * sharpness gate needs it — that gate changes pixel output, so it
     * must not silently die with the diagnostic-only probe option. */
    p_sys->probe.advice  = InheritIntSat( p_filter,
                                               CFG_PREFIX "content-probe" ) != 0;
    p_sys->probe.enabled = ( p_sys->probe.advice
                          || ( p_sys->usm_sharp_threshold > 0 && usm_pct > 0 ) )
                       && ChromaHasYPlane( chroma );
    p_sys->probe.active  = p_sys->probe.enabled;
    p_sys->probe.advice_logged = 0;
}

/* Wire fmt_out to the upscale target. Same chroma, new dimensions. */
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

static void ClampConfig( int *preset, int *algo, int *backend, int *usm, int *skip )
{
    /* SEC-2: Clamp config inputs to prevent resource exhaustion or logic errors. */
    *preset = up_normalize_auto_enum( *preset, UP_TARGET_MAX );
    if( *algo < 0 ) *algo = 0; else if( *algo > UP_ALGO_MAX ) *algo = UP_ALGO_MAX;
    *backend = up_normalize_auto_enum( *backend, SCALER_BACKEND_MAX );
    if( *usm < 0 ) *usm = 0; else if( *usm > UP_USM_AMOUNT_MAX ) *usm = UP_USM_AMOUNT_MAX;
    if( *skip < 0 ) *skip = 0;
}

/* OBS-5: the exported stat variables, in creation order. Kept as one table so
 * CreateStatsVars / the MaybeLogStats export / the Close teardown all agree on
 * the set; the names are documented in README under "Exported VLC variables". */
enum stats_var_id {
    STAT_VAR_EWMA_US,
    STAT_VAR_FRAMES,
    STAT_VAR_DROPPED,
    STAT_VAR_COUNT,
};

static const char *const k_stats_vars[STAT_VAR_COUNT] = {
    [STAT_VAR_EWMA_US] = "autoupscale-ewma-us",
    [STAT_VAR_FRAMES] = "autoupscale-frames",
    [STAT_VAR_DROPPED] = "autoupscale-dropped",
};

/* OBS-5: expose performance and frame stats via VLC variables.
 * ERR-1: on failure the export is disabled rather than var_SetInteger
 * silently operating on a nonexistent variable; any variables already created
 * are rolled back so Close() can gate every var_Destroy on the same flag.
 * Returns 1 only when the whole set exists. */
static int CreateStatsVars( filter_t *p_filter )
{
    for( size_t i = 0; i < STAT_VAR_COUNT; i++ )
    {
        if( var_Create( p_filter, k_stats_vars[i],
                        VLC_VAR_INTEGER ) == VLC_SUCCESS )
            continue;
        for( size_t j = 0; j < i; j++ )
            var_Destroy( p_filter, k_stats_vars[j] );
        msg_Info( p_filter,
                  "AutoUpscale: stat variable creation failed; "
                  "autoupscale-ewma-us/-frames/-dropped export disabled" );
        return 0;
    }
    return 1;
}

/* CX-1: reject CPUs below the build's -march LEVEL. This whole object is
 * compiled at that level, so the headline feature alone does not cover
 * BMI2/FMA/... (v3) or AVX512VL/... (v4) instructions the compiler is free
 * to emit anywhere in the plugin (BUILD-5/PORT-6). Returns VLC_SUCCESS when
 * the CPU is adequate (always, off x86-64). Extracted from Open() so the two
 * `#if` decision points don't count against Open's CCN. */
static int CheckCpuLevel( vlc_object_t *p_this )
{
    (void)p_this;
#if defined(__x86_64__)
# if defined(__AVX512F__)
    if (!up_cpu_supports_v4()) {
        msg_Err(p_this, "AutoUpscale: CPU below x86-64-v4 required by this build");
        return VLC_EGENERIC;
    }
# elif defined(__AVX2__)
    if (!up_cpu_supports_v3()) {
        msg_Err(p_this, "AutoUpscale: CPU below x86-64-v3 required by this build");
        return VLC_EGENERIC;
    }
# endif
#endif
    return VLC_SUCCESS;
}

/* CX-1: the one-shot "engaged" diagnostic. Every value but preset/cores/mem_mb
 * is already on p_sys by the time Open reaches this point, so the banner needs
 * no wide parameter list. Extracted from Open() to cut its physical length. */
static void LogEngaged( filter_t *p_filter, const filter_sys_t *p_sys,
                        int preset, int cores, unsigned long mem_mb )
{
    const scaler_ctx_t *sc = &p_sys->scaler;
    int threads_resolved = up_threads_decide( sc->threads_pref, cores );

    msg_Info( p_filter,
              "AutoUpscale engaged: %dx%d -> %dx%d "
              "(backend=%s preset=%d algo=%d usm=%d fps_target=%d "
              "threads_budget=%d cores=%d mem=%luMB simd=%s)",
              sc->src_w, sc->src_h, sc->dst_w, sc->dst_h,
              sc->backend->name,
              preset, p_sys->algo, p_sys->usm_pct, p_sys->target_fps,
              threads_resolved, cores, mem_mb, up_usm_pool_variant_name );
}

static int Open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;

    if( CheckCpuLevel( p_this ) != VLC_SUCCESS )
        return VLC_EGENERIC;

    source_crop_t src = ResolveSourceCrop( p_filter );

    int skip_above   = InheritIntSat( p_filter, CFG_PREFIX "skip-above" );
    int preset       = InheritIntSat( p_filter, CFG_PREFIX "target" );
    int algo         = InheritIntSat( p_filter, CFG_PREFIX "algo" );
    int backend_pref = InheritIntSat( p_filter, CFG_PREFIX "backend" );
    int usm_pct      = InheritIntSat( p_filter, CFG_PREFIX "usm" );

    ClampConfig( &preset, &algo, &backend_pref, &usm_pct, &skip_above );

    int cores;
    unsigned long mem_mb;
    DetectHardware( &cores, &mem_mb );

    up_dims_t target = { 0, 0 };
    if( !up_plan_upscale( src.dims.width, src.dims.height, skip_above, preset,
                          cores, mem_mb, &target ) )
    {
        msg_Dbg( p_filter,
                 "AutoUpscale: bypassing %dx%d (skip>=%d, preset=%d)",
                 src.dims.width, src.dims.height, skip_above, preset );
        return VLC_EGENERIC;
    }

    const vlc_fourcc_t chroma = p_filter->fmt_in.video.i_chroma;
    const scaler_backend_t *be = PickBackendOrReject( p_filter, chroma,
                                                     algo, backend_pref );
    if( !be )
        return VLC_EGENERIC;

    filter_sys_t *p_sys = calloc( 1, sizeof(*p_sys) );
    if( !p_sys ) return VLC_ENOMEM;

    p_sys->backend_pref = backend_pref;   /* SYS-2: fallback respects it */
    ConfigureScaler( &p_sys->scaler, be, p_filter, chroma, algo,
                     src, target );

    if( OpenScalerOrFallback( p_filter, p_sys ) != 0 )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    InitUsmPool( p_sys, p_filter, chroma, target, cores, usm_pct );
    InitProbeAndPerfmon( p_sys, p_filter, chroma, algo, usm_pct );
    SetOutputFormat( p_filter, chroma, target );

    p_sys->stats_vars_ok = CreateStatsVars( p_filter );

    p_filter->p_sys           = p_sys;
    p_filter->pf_video_filter = Filter;

    LogEngaged( p_filter, p_sys, preset, cores, mem_mb );

    return VLC_SUCCESS;
}

/* Read CLOCK_MONOTONIC in nanoseconds. Falls back to 0 on failure (perfmon
 * ignores non-positive samples). */
static inline int64_t monotonic_ns(void)
{
    struct timespec ts;
    if( clock_gettime( CLOCK_MONOTONIC, &ts ) != 0 ) return 0;
    if( ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L )
        return 0;

    const uintmax_t seconds = (uintmax_t)ts.tv_sec;
    const uintmax_t nanoseconds = (uintmax_t)ts.tv_nsec;
    const uintmax_t scale = UINTMAX_C( 1000000000 );
    if( seconds > ( (uintmax_t)INT64_MAX - nanoseconds ) / scale )
        return 0;
    return (int64_t)( seconds * scale + nanoseconds );
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
static void EmitPerfAdvisory( filter_t *p_filter, const filter_sys_t *p_sys )
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
    if( p_sys->scaler.backend->id != SCALER_BACKEND_SWSCALE )
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
 * Filter() while p_sys->probe.active is true and p_in has a readable luma
 * plane (the probe.enabled gate in Open() already filtered out formats
 * without one). The shared picture view
 * resolves VLC's visible-area crop and rejects malformed plane geometry.
 *
 * Extracted from Filter() to keep its cyclomatic complexity within
 * the project's complexity ceiling.
 */
static void LogProbeVerdict( filter_t *p_filter, filter_sys_t *p_sys );

static void DisableUsm( filter_sys_t *p_sys )
{
    p_sys->usm_amount_q8 = 0;
    up_usm_pool_destroy( p_sys->usm_pool );
    p_sys->usm_pool = NULL;
}

static void RunProbe( filter_t *p_filter, filter_sys_t *p_sys,
                      const picture_t *p_in )
{
    const scaler_ctx_t *sc = &p_sys->scaler;
    up_picture_view_t view;
    const up_picture_region_t region = up_scaler_src_region(sc);
    if( !up_picture_view_init( &view, p_in, sc->chroma, &region ) )
        return;

    const uint8_t *pixels = view.plane[0].pixels;
    const int pitch = view.plane[0].pitch;
    up_probe_metrics_t m;
    up_probe_metrics( pixels, pitch, sc->src_w, sc->src_h, &m );
    up_probe_observe( &p_sys->probe.accum, m.lap_sum, m.lap_n,
                      m.edge_sum, m.edge_n );

    if( p_sys->probe.accum.frames < UP_PROBE_WINDOW_FRAMES )
        return;

    p_sys->probe.active = 0;
    if( up_should_skip_usm_for_sharpness( &p_sys->probe.accum,
                                          p_sys->usm_sharp_threshold ) )
    {
        p_sys->usm_skip_sharp = 1;
        DisableUsm( p_sys );
        msg_Info( p_filter,
                  "AutoUpscale: source is heavily textured "
                  "(lap_mean=%llu > threshold=%d); skipping post-USM "
                  "to avoid amplifying grain. "
                  "Override via --autoupscale-usm-sharp-threshold=0 "
                  "(disable) or a different cutoff.",
                  (unsigned long long)(p_sys->probe.accum.lap_sum
                       / p_sys->probe.accum.lap_samples),
                  p_sys->usm_sharp_threshold );
    }
    if( p_sys->probe.advice )
        LogProbeVerdict( p_filter, p_sys );
}

/* Bypass-advisory verdict logging, split from RunProbe: it runs only
 * when the content-probe option is on, while the metric collection
 * above also serves the USM sharpness gate. */
static void LogProbeVerdict( filter_t *p_filter, filter_sys_t *p_sys )
{
    int recommend_bypass = up_should_bypass_for_content( &p_sys->probe.accum );
    uint64_t lap_mean  = p_sys->probe.accum.lap_samples
        ? p_sys->probe.accum.lap_sum  / p_sys->probe.accum.lap_samples : 0;
    uint64_t edge_mean = p_sys->probe.accum.edge_samples
        ? p_sys->probe.accum.edge_sum / p_sys->probe.accum.edge_samples : 0;

    if( recommend_bypass && !p_sys->probe.advice_logged )
    {
        p_sys->probe.advice_logged = 1;
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

/* Apply the post-pass USM in-place on the cropped luma plane, when enabled.
 * No-op when amount==0, no pool allocated, no luma plane, or invalid output
 * geometry. A pool failure (sticky lazy-init, OBS parity with the zimg
 * pool's OBS-2)
 * warns once and disables USM for the rest of playback so every later
 * frame skips the dead call. */
static int ApplyUsmIfEnabled( filter_t *p_filter, filter_sys_t *p_sys,
                              const picture_t *p_out )
{
    if( p_sys->usm_amount_q8 <= 0 || !p_sys->usm_pool
        || p_out->i_planes < 1 || p_sys->usm_skip_sharp )
        return UP_USM_APPLY_OK;

    const scaler_ctx_t *sc = &p_sys->scaler;
    up_picture_view_t view;
    const up_picture_region_t region = up_scaler_dst_region(sc);
    if( !up_picture_view_init( &view, p_out, sc->chroma, &region ) )
        return UP_USM_APPLY_OK;

    uint8_t *pixels = view.plane[0].pixels;
    const int pitch = view.plane[0].pitch;
    const int status = up_usm_pool_apply(
            p_sys->usm_pool,
            pixels, pitch,
            pixels, pitch,        /* in-place */
            p_sys->usm_amount_q8 );
    if( status != UP_USM_APPLY_OK )
    {
        msg_Info( p_filter,   /* OBS-2: msg_Warn is suppressed by default */
                  "AutoUpscale: USM pool initialization or dispatch failed; "
                  "sharpening disabled for this playback" );
        DisableUsm( p_sys );
        return status;
    }
    if( !p_sys->usm_workers_logged )
    {
        p_sys->usm_workers_logged = 1;
        const int workers =
            up_usm_pool_effective_threads( p_sys->usm_pool );
        msg_Info( p_filter,
                  "AutoUpscale: USM pool running %d worker%s",
                  workers, workers == 1 ? "" : "s" );
    }
    return UP_USM_APPLY_OK;
}

/* OBS-3: every OBS_STATS_INTERVAL_NS, log a one-line long-run summary so
 * sustained behavior is observable beyond the one-shot perf advisory. */
#define OBS_STATS_INTERVAL_NS (5 * 1000000000LL)
static void MaybeLogStats( filter_t *p_filter, filter_sys_t *p_sys,
                           int64_t now_ns )
{
    if( now_ns <= 0 )
        return;
    if( p_sys->last_stats_ns <= 0 || now_ns < p_sys->last_stats_ns )
    {
        p_sys->last_stats_ns = now_ns;
        return;
    }
    const uint64_t elapsed_ns = (uint64_t)now_ns
                              - (uint64_t)p_sys->last_stats_ns;
    if( elapsed_ns < (uint64_t)OBS_STATS_INTERVAL_NS )
        return;
    p_sys->last_stats_ns = now_ns;
    msg_Dbg( p_filter,
             "AutoUpscale: frames=%llu dropped=%llu ewma=%ldus%s",
             (unsigned long long)p_sys->frame_count,
             (unsigned long long)p_sys->dropped_count,
             (long)up_perfmon_ewma_us( &p_sys->perfmon ),
             p_sys->usm_skip_sharp ? " usm=skipped(sharp)" : "" );

    /* OBS-5: exported stat variables ride the same tick — string-keyed
     * var_SetInteger takes the object var lock, too heavy per frame. */
    if( p_sys->stats_vars_ok )
    {
        var_SetInteger( p_filter, k_stats_vars[STAT_VAR_EWMA_US],
                        (int64_t)up_perfmon_ewma_us( &p_sys->perfmon ) );
        var_SetInteger( p_filter, k_stats_vars[STAT_VAR_FRAMES],
                        p_sys->frame_count );
        var_SetInteger( p_filter, k_stats_vars[STAT_VAR_DROPPED],
                        p_sys->dropped_count );
    }
}

/* Record one frame's elapsed work into the perfmon and emit the one-time
 * advisory if perfmon decides the budget has been blown. */
static void RecordPerf( filter_t *p_filter, filter_sys_t *p_sys,
                        int64_t t_start, int64_t t_end )
{
    int64_t elapsed_ns = (t_start > 0 && t_end > t_start) ? t_end - t_start : 0;
    if( up_perfmon_record_ns( &p_sys->perfmon, elapsed_ns ) )
        EmitPerfAdvisory( p_filter, p_sys );

    p_sys->frame_count++;
    MaybeLogStats( p_filter, p_sys, t_end );
}

/* OBS-1: count a dropped frame AND drive the stats tick. MaybeLogStats used to
 * hang off RecordPerf, which only runs on the success path — so once a fatal
 * backend failure started dropping every frame, the periodic "frames=…
 * dropped=…" line stopped and var_SetInteger("autoupscale-dropped", …) was
 * never called again. An embedder polling that variable read 0 while 100% of
 * frames were being dropped: the counter went blind in exactly the scenario it
 * exists for. */
static void RecordDrop( filter_t *p_filter, filter_sys_t *p_sys )
{
    p_sys->dropped_count++;
    MaybeLogStats( p_filter, p_sys, monotonic_ns() );
}

static picture_t *FinishScaledFrame( filter_t *p_filter, filter_sys_t *p_sys,
                                     picture_t *p_in, picture_t *p_out,
                                     int64_t t_start )
{
    if( ApplyUsmIfEnabled( p_filter, p_sys, p_out )
        == UP_USM_APPLY_OUTPUT_UNCERTAIN )
    {
        RecordDrop( p_filter, p_sys );
        picture_Release( p_out );
        picture_Release( p_in );
        return NULL;
    }

    RecordPerf( p_filter, p_sys, t_start, monotonic_ns() );
    picture_CopyProperties( p_out, p_in );
    picture_Release( p_in );
    return p_out;
}

/* SYS-2: one-shot runtime fallback to swscale after the active backend
 * reports a fatal processing failure. zimg defers its heavy setup
 * (worker spawn, scratch alloc, per-cell graph build) to the first valid frame,
 * so a lazy-init failure (OOM under pressure, graph-build edge case) is
 * sticky: Open() already succeeded, VLC committed to this filter, and
 * without a swap every frame of the playback would be dropped and the
 * broad-coverage swscale fallback would never engage. The
 * scaler_ctx_t geometry is backend-agnostic, so closing zimg and opening
 * swscale on the same ctx is a clean swap; one frame is dropped. A
 * forced --autoupscale-backend=1 (zimg) is respected: the failed backend is
 * retired without opening swscale. On a failed swap the backend is left NULL
 * and Filter() drops every frame — same behavior as before, minus the dead
 * process() call. */
static void RetireBackend( scaler_ctx_t *ctx )
{
    ctx->backend->close( ctx );
    ctx->priv = NULL;
    ctx->backend = NULL;
}

static void TryBackendFallback( filter_t *p_filter, filter_sys_t *p_sys )
{
    if( p_sys->fallback_tried )
        return;
    p_sys->fallback_tried = 1;

    scaler_ctx_t *ctx = &p_sys->scaler;
    if( ctx->backend->id != SCALER_BACKEND_ZIMG )
        return;
    if( p_sys->backend_pref == SCALER_BACKEND_ZIMG )
    {
        /* OBS-2: without this the user sees only the generic one-shot
         * "failed to process a frame" warning, with no hint that their
         * forced-backend setting is what suppressed recovery. */
        msg_Err( p_filter,
                 "AutoUpscale: zimg failed fatally but "
                 "--autoupscale-backend forces zimg; swscale fallback "
                 "suppressed, all remaining frames will be dropped" );
        RetireBackend( ctx );
        return;
    }

    const scaler_backend_t *sw = scaler_pick( SCALER_BACKEND_SWSCALE,
                                              ctx->chroma, ctx->algo );
    RetireBackend( ctx );
    if( !sw || sw->open( ctx ) != 0 )
    {
        msg_Err( p_filter,
                 "AutoUpscale: swscale fallback open failed; "
                 "dropping all frames for this playback" );
        return;
    }
    ctx->backend = sw;
    msg_Info( p_filter,   /* OBS-2: msg_Warn is suppressed by default */
              "AutoUpscale: zimg failed at runtime; "
              "fell back to swscale for the rest of this playback" );
}

static picture_t *Filter( filter_t *p_filter, picture_t *p_in )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    if( !p_in ) return NULL;

    /* SYS-2: a failed backend fallback leaves no live backend. */
    if( !p_sys->scaler.backend )
    {
        RecordDrop( p_filter, p_sys );
        picture_Release( p_in );
        return NULL;
    }

    if( p_sys->probe.active && p_in->i_planes >= 1 )
        RunProbe( p_filter, p_sys, p_in );

    picture_t *p_out = filter_NewPicture( p_filter );
    if( !p_out )
    {
        if( !p_sys->newpic_fail_logged )
        {
            p_sys->newpic_fail_logged = 1;
            /* OBS-2: msg_Info, not msg_Warn — VLC 3.x's default verbosity
             * suppresses level-2 warnings, so the user whose playback is
             * stuttering from pool exhaustion would see nothing at all. The
             * latch keeps it one-shot, so there is no spam risk. */
            msg_Info( p_filter,
                      "AutoUpscale: output picture pool exhausted; "
                      "dropping frame(s) (this is logged only once)" );
        }
        RecordDrop( p_filter, p_sys );
        picture_Release( p_in );
        return NULL;
    }

    int64_t t_start = monotonic_ns();

    scaler_process_status_t status = p_sys->scaler.backend->process(
        &p_sys->scaler, p_in, p_out );
    if( status != SCALER_PROCESS_OK )
    {
        if( !p_sys->process_fail_logged )
        {
            p_sys->process_fail_logged = 1;
            msg_Info( p_filter,   /* OBS-2: see the pool-exhaustion note */
                      "AutoUpscale: %s backend failed to process a frame; "
                      "dropping frame(s) (this is logged only once)",
                      p_sys->scaler.backend->name );
        }
        if( scaler_process_needs_fallback( status ) )
            TryBackendFallback( p_filter, p_sys );
        RecordDrop( p_filter, p_sys );
        picture_Release( p_out );
        picture_Release( p_in );
        return NULL;
    }

    return FinishScaledFrame( p_filter, p_sys, p_in, p_out, t_start );
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

        /* OBS-5: pair the var_Create in Open(); ERR-1 gates both on the
         * pair having been created successfully. */
        if( p_sys->stats_vars_ok )
        {
            for( size_t i = 0; i < STAT_VAR_COUNT; i++ )
                var_Destroy( p_filter, k_stats_vars[i] );
        }
        free( p_sys );
    }
}
