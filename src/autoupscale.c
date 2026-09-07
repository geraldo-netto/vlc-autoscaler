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
#include <vlc_filter.h>
#include <vlc_picture.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "autoupscale_module.h"
#include "upscale_logic.h"
#include "usm.h"
#include "usm_pool.h"
#include "usm_adaptive.h"
#include "scaler.h"
#include "scaler_pick_logic.h"
#include "thread_policy.h"
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

static picture_t *Filter( filter_t *, picture_t * );

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
    up_usm_adaptive_t usm_adaptive;

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
     *   advice        : 1 if the bypass advisory may be logged
     *                   (content-probe option only)
     *   active        : 1 while metric collection is needed and still active
     *   advice_logged : 1 once we've logged the bypass recommendation
     *   accum         : accumulator state passed to up_probe_observe()
     */
    struct
    {
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

    /* One-shot log of the USM pool's real worker count, deferred to after
     * the first apply() because lazy init may shrink it (the engagement
     * log's threads= reflects neither pool). */
    int                usm_workers_logged;
};

/*****************************************************************************
 * Helpers
 *****************************************************************************/

/*****************************************************************************
 * Open: probe input format, decide whether to engage, set up scaler + USM
 *****************************************************************************
 * Open() is split into focused phase helpers:
 *   ResolveSourceCrop     — picks and aligns visible-or-physical source crop
 *   PickBackendOrReject   — opaque-chroma + null-backend gate
 *   ConfigureScaler       — fills scaler_ctx_t from VLC vars
 *   InitUsmPool           — optional USM post-pass setup
 *   InitProbe             — content-probe bookkeeping
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
        /* OBS-2: msg_Warn is suppressed at VLC's default verbosity; the
         * reason for the quality downgrade must be visible one-shot. */
        msg_Info( p_filter,
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
    int threads = InheritIntSat( p_filter, UP_CFG_PREFIX "threads" );
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
        UP_CFG_PREFIX "pin-threads" ) ? 1 : 0;
    sc->zimg.min_stripe_lines = InheritIntSat( p_filter,
        UP_CFG_PREFIX "zimg-stripe-lines" );
    sc->zimg.zerocopy = InheritIntSat( p_filter,
        UP_CFG_PREFIX "zerocopy-dst" );
    sc->zimg.src_zerocopy = InheritIntSat( p_filter,
        UP_CFG_PREFIX "zerocopy-src" );
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

static void InitAdaptiveUsm( filter_sys_t *p_sys, filter_t *p_filter,
                              int initial, int cores, up_dims_t target,
                              int stripe_min )
{
    if( !InheritIntSat( p_filter, UP_CFG_PREFIX "adaptive-usm" ) ) return;
    if( p_sys->scaler.threads_pref != UP_THREADS_AUTO )
    {
        msg_Info( p_filter, "Adaptive USM requires threads=0; keeping explicit count" );
        return;
    }
    up_usm_adaptive_init( &p_sys->usm_adaptive, initial,
        up_threads_decide( UP_THREADS_MAX, cores ),
        target.width, target.height, stripe_min );
    msg_Info( p_filter, "Adaptive USM: minimizing processing time, 1..%d workers",
              p_sys->usm_adaptive.tuner.limit );
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

    int n_threads = up_usm_threads_decide( p_sys->scaler.threads_pref, cores,
                                          target.width, target.height );
    int stripe_min = InheritIntSat( p_filter,
        UP_CFG_PREFIX "usm-stripe-min-rows" );
    p_sys->usm_pool = up_usm_pool_create( n_threads,
                                          target.width, target.height,
                                          stripe_min );
    if( p_sys->usm_pool )
    {
        p_sys->usm_amount_q8 = up_usm_amount_pct_to_q8( usm_pct );
        InitAdaptiveUsm( p_sys, p_filter, n_threads, cores, target, stripe_min );
    }
    else
        msg_Info( p_filter,
                  "USM pool create failed (%dx%d, %d threads); "
                  "sharpening disabled",
                  target.width, target.height, n_threads );
}

/* Initialize the content-probe bookkeeping fields. */
static void InitProbe( filter_sys_t *p_sys, filter_t *p_filter,
                       vlc_fourcc_t chroma, int usm_pct )
{
    /* Content-aware metric probe. Runs only when the source exposes a
     * readable luma plane. For opaque and packed RGB chromas the probe stays
     * disabled because they are GPU-managed or have no discrete luma plane.
     * The accumulator is already zeroed by calloc(). */
    p_sys->usm_sharp_threshold = InheritIntSat( p_filter,
        UP_CFG_PREFIX "usm-sharp-threshold" );

    /* Metric collection also runs with content-probe=0 when the USM
     * sharpness gate needs it — that gate changes pixel output, so it
     * must not silently die with the diagnostic-only probe option. */
    p_sys->probe.advice  = InheritIntSat( p_filter,
        UP_CFG_PREFIX "content-probe" ) != 0;
    p_sys->probe.active = ( p_sys->probe.advice
                         || ( p_sys->usm_sharp_threshold > 0 && usm_pct > 0 ) )
                      && ChromaHasYPlane( chroma );
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

/* CX-1: the one-shot "engaged" diagnostic. Every value but preset/cores
 * is already on p_sys by the time Open reaches this point, so the banner needs
 * no wide parameter list. Extracted from Open() to cut its physical length. */
static void LogEngaged( filter_t *p_filter, const filter_sys_t *p_sys,
                        int preset, int cores )
{
    const scaler_ctx_t *sc = &p_sys->scaler;
    int threads_resolved = up_threads_decide( sc->threads_pref, cores );

    msg_Info( p_filter,
              "AutoUpscale engaged: %dx%d -> %dx%d "
              "(backend=%s preset=%d algo=%d "
              "threads_budget=%d cores=%d simd=%s)",
              sc->src_w, sc->src_h, sc->dst_w, sc->dst_h,
              sc->backend->name,
              preset, sc->algo,
              threads_resolved, cores, up_usm_pool_variant_name );
}

int up_autoupscale_open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;

    source_crop_t src = ResolveSourceCrop( p_filter );

    int skip_above = InheritIntSat( p_filter,
                                    UP_CFG_PREFIX "skip-above" );
    int preset = InheritIntSat( p_filter, UP_CFG_PREFIX "target" );
    int algo = InheritIntSat( p_filter, UP_CFG_PREFIX "algo" );
    int backend_pref = InheritIntSat( p_filter, UP_CFG_PREFIX "backend" );
    int usm_pct = InheritIntSat( p_filter, UP_CFG_PREFIX "usm" );

    ClampConfig( &preset, &algo, &backend_pref, &usm_pct, &skip_above );

    const int cores = up_detect_cores();

    up_dims_t target = { 0, 0 };
    if( !up_plan_upscale( src.dims.width, src.dims.height, skip_above, preset,
                          cores, 0, &target ) )
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
    InitProbe( p_sys, p_filter, chroma, usm_pct );
    SetOutputFormat( p_filter, chroma, target );

    p_filter->p_sys           = p_sys;
    p_filter->pf_video_filter = Filter;

    LogEngaged( p_filter, p_sys, preset, cores );

    return VLC_SUCCESS;
}

/* Filter: scale one picture, then apply USM when enabled. */

/*
 * Run one iteration of the content probe on the source frame, and
 * close the probe (logging an advisory) once the window is full.
 * Observe-only — does not modify p_in or any output. Called from
 * Filter() while p_sys->probe.active is true and p_in has a readable luma
 * plane (the probe.active gate in Open() already filtered out formats
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
    up_usm_adaptive_stop( &p_sys->usm_adaptive );
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

static int RunUsm( filter_t *p_filter, filter_sys_t *p_sys,
                    uint8_t *pixels, int pitch )
{
    up_usm_adaptive_t *a = &p_sys->usm_adaptive;
    const bool enabled = a->enabled;
    const unsigned changes = a->tuner.changes;
    const up_tuner_phase_t phase = a->tuner.phase;
    const int status = up_usm_adaptive_apply( a, &p_sys->usm_pool,
        pixels, pitch, p_sys->usm_amount_q8 );
    if( enabled && a->stopped )
        msg_Info( p_filter, "Adaptive USM stopped; retaining working pool" );
    if( changes != a->tuner.changes )
        msg_Info( p_filter, "Adaptive USM: selected %d workers", a->tuner.best );
    if( phase != UP_TUNER_SETTLED && a->tuner.phase == UP_TUNER_SETTLED )
        msg_Info( p_filter, "Adaptive USM: settled on %d workers, %.1f us/frame",
                  a->tuner.best, a->tuner.steady_us );
    return status;
}

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
    const int status = RunUsm( p_filter, p_sys, pixels, pitch );
    if( status != UP_USM_APPLY_OK )
    {
        if( p_sys->usm_adaptive.retained ) return status;
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

static picture_t *FinishScaledFrame( filter_t *p_filter, filter_sys_t *p_sys,
                                     picture_t *p_in, picture_t *p_out )
{
    if( ApplyUsmIfEnabled( p_filter, p_sys, p_out )
        == UP_USM_APPLY_OUTPUT_UNCERTAIN )
    {
        picture_Release( p_out );
        picture_Release( p_in );
        return NULL;
    }

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
    if( sw )
    {
        ctx->backend = sw;
        if( sw->open( ctx ) == 0 )
        {
            msg_Info( p_filter,   /* OBS-2: msg_Warn is suppressed by default */
                      "AutoUpscale: zimg failed at runtime; "
                      "fell back to swscale for the rest of this playback" );
            return;
        }
        ctx->backend = NULL;
    }
    msg_Err( p_filter,
             "AutoUpscale: swscale fallback open failed; "
             "dropping all frames for this playback" );
}

static picture_t *Filter( filter_t *p_filter, picture_t *p_in )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    if( !p_in ) return NULL;

    /* SYS-2: a failed backend fallback leaves no live backend. */
    if( !p_sys->scaler.backend )
    {
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
        picture_Release( p_in );
        return NULL;
    }

    up_usm_adaptive_begin( &p_sys->usm_adaptive );
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
        picture_Release( p_out );
        picture_Release( p_in );
        return NULL;
    }

    return FinishScaledFrame( p_filter, p_sys, p_in, p_out );
}

/*****************************************************************************
 * Close: tear down
 *****************************************************************************/
void up_autoupscale_close( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    if( p_sys )
    {
        if( p_sys->scaler.backend )
            p_sys->scaler.backend->close( &p_sys->scaler );
        DisableUsm( p_sys );

        free( p_sys );
    }
}
