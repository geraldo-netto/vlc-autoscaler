// SPDX-License-Identifier: GPL-2.0-or-later

#include <vlc_common.h>
#include <vlc_plugin.h>

#include "autoupscale_module.h"
#include "content_probe.h"
#include "cpu_level.h"
#include "scaler.h"
#include "thread_policy.h"
#include "upscale_logic.h"
#include "usm.h"

#ifndef N_
# define N_(s) (s)
#endif

#ifndef UP_REQUIRED_CPU_LEVEL
# define UP_REQUIRED_CPU_LEVEL 0
#endif

#if UP_REQUIRED_CPU_LEVEL != 0 && UP_REQUIRED_CPU_LEVEL != 3 \
    && UP_REQUIRED_CPU_LEVEL != 4
# error "UP_REQUIRED_CPU_LEVEL must be 0, 3, or 4"
#endif

#ifndef UP_CPU_SUPPORTS_V3
# define UP_CPU_SUPPORTS_V3() up_cpu_supports_v3()
#endif

#ifndef UP_CPU_SUPPORTS_V4
# define UP_CPU_SUPPORTS_V4() up_cpu_supports_v4()
#endif

#define TARGET_TEXT     N_("Target resolution")
#define TARGET_LONGTEXT N_( \
    "0 = auto (decide between 720p and 1080p based on CPU), " \
    "1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 4K (2160p), " \
    "5 = 5K (2880p), 6 = 8K (4320p). " \
    "Targets above 1080p require explicit selection. " \
    "All targets are subject to the 4x linear ratio cap relative to source.")

#define ALGO_TEXT       N_("Scaling algorithm")
#define ALGO_LONGTEXT   N_( \
    "0 = fast bilinear (cheapest), " \
    "1 = bicubic (balanced), " \
    "2 = lanczos, " \
    "3 = spline36 (default; best for upscaling; zimg only — falls back to " \
    "lanczos on swscale).")

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

#define THREADS_TEXT    N_("Worker preference for zimg and USM")
#define THREADS_LONGTEXT N_( \
    "0 = auto: zimg uses cores/2 - 2, clamped to [1,12]; USM uses 8 " \
    "workers up to 1280x720 output pixels, 12 up to 1920x1080, and " \
    "16 above that. 1..64 = explicit preference for both pools, capped by CPUs " \
    "allowed to the process. Each pool may clamp lower for frame geometry. " \
    "The zimg backend normally uses horizontal stripes and adds column " \
    "tiles for very wide/short frames. It runs one persistent worker " \
    "per grid cell; swscale remains single-threaded. Higher values can " \
    "reduce per-frame latency at the cost of more memory and lower " \
    "per-thread cache locality. USM AUTO is also capped by allowed CPUs; " \
    "only zimg AUTO applies the half-CPU reserve. Override if you " \
    "measured otherwise. With adaptive-usm=1, USM may explore beyond this " \
    "resolution-based AUTO count; zimg keeps its fixed grid.")

#define ADAPTIVE_USM_TEXT N_("Adapt USM workers to minimize time per frame")
#define ADAPTIVE_USM_LONGTEXT N_( \
    "Experimental measured worker search (0=off, 1=on). Requires threads=0 " \
    "and active sharpening. Tries 1..64 workers within CPU and stripe limits; " \
    "keeps confirmed faster counts. Trials add temporary latency and resource " \
    "cost. Resolution, algorithm and zimg grid stay fixed.")

#define PIN_TEXT        N_("Pin scaler worker threads to CPU cores")
#define PIN_LONGTEXT    N_( \
    "0 = off (let the OS scheduler place threads). 1 = on (default): pin " \
    "each zimg scaler worker thread to a distinct CPU core (round-robin). " \
    "Linux only; best-effort (ignored if it fails). Disable if pinning " \
    "hurts by fighting VLC's other threads or the scheduler's load " \
    "balancing. Does not affect the USM sharpening pool.")

#define ZEROCOPY_DST_TEXT N_("Write directly to VLC's destination picture")
#define ZEROCOPY_DST_LONGTEXT N_( \
    "1 = on (default): on aligned row-only grids, workers write directly " \
    "into VLC's destination picture, skipping copy-out. Column grids " \
    "always use per-tile scratch. This avoids a frame-sized copy and its " \
    "persistent plane scratch. 0 = off (safe fallback): worker threads " \
    "write to plugin-owned scratch buffers and copy their output regions " \
    "into VLC's destination picture. Set to 0 if you see garbled output, " \
    "crashes, or other instability with the default - the writeback to " \
    "VLC's destination picture has been verified byte-identical to the " \
    "copy-out path in our testing but cannot be fully verified across " \
    "every VLC build configuration.")

#define ZEROCOPY_SRC_TEXT N_("Read VLC's source picture directly")
#define ZEROCOPY_SRC_LONGTEXT N_( \
    "1 = on (default): zimg worker threads read VLC's source picture " \
    "directly, skipping copy-in and its persistent plane scratch. The " \
    "symmetric twin of zerocopy-dst (which writes VLC's destination " \
    "picture directly). 0 = off (safe fallback): the source is copied to " \
    "plugin-owned scratch first, then the worker graphs read the scratch. " \
    "On very wide/short frames setting 0 also disables column tiling " \
    "(fewer workers), since tiles need direct source reads. Set to 0 if " \
    "you see garbled output, crashes, or instability - reading VLC's " \
    "pool-managed source buffers from worker threads has been verified " \
    "byte-identical to the copy-in path in our harness but, like " \
    "zerocopy-dst, cannot be fully verified across every VLC build " \
    "configuration.")

#define USM_STRIPE_MIN_ROWS_TEXT N_("Minimum rows per USM stripe")
#define USM_STRIPE_MIN_ROWS_LONGTEXT N_( \
    "Each USM worker thread processes at least this many rows. " \
    "Smaller values let more workers fit on low-resolution frames " \
    "(e.g. 480p) but increase per-frame thread-dispatch overhead. " \
    "0 = auto (8, matches the kernel boundary handling). Range 0..256.")

#define ZIMG_STRIPE_LINES_TEXT N_("Minimum dst lines per zimg stripe")
#define ZIMG_STRIPE_LINES_LONGTEXT N_( \
    "Each zimg worker thread emits at least this many destination " \
    "rows. Smaller values let more workers fit on low-res output but " \
    "increase per-stripe boundary work. 0 = auto (16). Range 0..128.")

#define USM_SHARP_THRESH_TEXT N_("USM-skip sharpness threshold")
#define USM_SHARP_THRESH_LONGTEXT N_( \
    "Mean squared Laplacian-response value above which the source is " \
    "considered heavily textured/grainy and the USM post-pass is skipped " \
    "for the rest of playback (USM on grainy content amplifies noise " \
    "without adding perceived sharpness). Lower = trip more aggressively " \
    "(skip USM on more sources). Higher = trip rarely (USM stays on most " \
    "content). 0 = feature off, USM always runs regardless of source. " \
    "Independent of --autoupscale-content-probe: the sharpness metric is " \
    "collected even with the probe advisory disabled. Default 3500. " \
    "Range 0..20000.")

#define PROBE_TEXT N_("Content-aware quality probe")
#define PROBE_LONGTEXT N_( \
    "1 = on (default): observe a fixed initial luma window to estimate " \
    "source quality. If the source is both very soft (low mean squared " \
    "Laplacian response: heavy blur or noise reduction) AND very blocky " \
    "(high edge intensity at 8-pixel boundaries: heavy compression), log " \
    "a one-time advisory recommending the user disable AutoUpscale for " \
    "this source. The probe is DIAGNOSTIC only — VLC 3's filter API does " \
    "not allow runtime format renegotiation, so the filter cannot " \
    "self-bypass mid-stream. The fixed sampling step spans the visible " \
    "plane, so cost scales with its dimensions and stops after the probe " \
    "window. 0 = off: no advisory is logged (metric collection still runs " \
    "when --autoupscale-usm-sharp-threshold needs it, since that gate " \
    "changes pixel output and is not diagnostic). The probe is skipped " \
    "automatically for chromas without a readable Y plane.")

static int CheckCpuLevel( vlc_object_t *p_this )
{
    (void)p_this;
#if defined(__x86_64__) && UP_REQUIRED_CPU_LEVEL == 4
    if( !UP_CPU_SUPPORTS_V4() )
    {
        msg_Err( p_this,
                 "AutoUpscale: CPU below x86-64-v4 required by this build" );
        return VLC_EGENERIC;
    }
#elif defined(__x86_64__) && UP_REQUIRED_CPU_LEVEL == 3
    if( !UP_CPU_SUPPORTS_V3() )
    {
        msg_Err( p_this,
                 "AutoUpscale: CPU below x86-64-v3 required by this build" );
        return VLC_EGENERIC;
    }
#endif
    return VLC_SUCCESS;
}

__attribute__((noinline, used))
static int up_autoupscale_open_checked( vlc_object_t *p_this )
{
    if( CheckCpuLevel( p_this ) != VLC_SUCCESS )
        return VLC_EGENERIC;
    return up_autoupscale_open( p_this );
}

static void Close( vlc_object_t *p_this )
{
    up_autoupscale_close( p_this );
}

vlc_module_begin()
    set_shortname( N_("AutoUpscale") )
    set_description( N_("Automatic sub-720p -> 720p/1080p video upscaler") )
    set_help( N_("Detects sub-720p video and upscales it to 720p or 1080p "
                 "in real time. Backends: zimg (preferred) and swscale.") )
    set_capability( "video filter", 0 )
    set_category( CAT_VIDEO )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    set_callbacks( up_autoupscale_open_checked, Close )
    add_shortcut( "autoupscale" )

    add_integer_with_range( UP_CFG_PREFIX "target", UP_TARGET_AUTO,
                            UP_TARGET_AUTO, UP_TARGET_MAX,
                            TARGET_TEXT, TARGET_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "algo", UP_ALGO_SPLINE36,
                            UP_ALGO_FAST_BILINEAR, UP_ALGO_MAX,
                            ALGO_TEXT, ALGO_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "skip-above", 720, 0, 8192,
                            SKIP_TEXT, SKIP_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "usm", UP_USM_AMOUNT_DEFAULT,
                            0, UP_USM_AMOUNT_MAX,
                            USM_TEXT, USM_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "backend", SCALER_BACKEND_AUTO,
                            SCALER_BACKEND_AUTO, SCALER_BACKEND_MAX,
                            BACKEND_TEXT, BACKEND_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "threads", UP_THREADS_AUTO,
                            0, UP_THREADS_MAX,
                            THREADS_TEXT, THREADS_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "adaptive-usm", 0, 0, 1,
                            ADAPTIVE_USM_TEXT, ADAPTIVE_USM_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "pin-threads", 1, 0, 1,
                            PIN_TEXT, PIN_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "zerocopy-dst", 1, 0, 1,
                            ZEROCOPY_DST_TEXT, ZEROCOPY_DST_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "zerocopy-src", 1, 0, 1,
                            ZEROCOPY_SRC_TEXT, ZEROCOPY_SRC_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "content-probe", 1, 0, 1,
                            PROBE_TEXT, PROBE_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "usm-stripe-min-rows", 0, 0, 256,
                            USM_STRIPE_MIN_ROWS_TEXT,
                            USM_STRIPE_MIN_ROWS_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "zimg-stripe-lines", 0, 0, 128,
                            ZIMG_STRIPE_LINES_TEXT,
                            ZIMG_STRIPE_LINES_LONGTEXT, false )
    add_integer_with_range( UP_CFG_PREFIX "usm-sharp-threshold",
                            UP_PROBE_THRESH_SHARP_LAP_MEAN, 0, 20000,
                            USM_SHARP_THRESH_TEXT,
                            USM_SHARP_THRESH_LONGTEXT, false )
vlc_module_end()
