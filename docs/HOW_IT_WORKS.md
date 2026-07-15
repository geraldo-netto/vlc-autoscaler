# How AutoUpscale works

This document explains what the plugin actually does at runtime, so you can
decide whether the trade-offs match what you want.

## Filter lifecycle in VLC

VLC video filters are loaded by the playback engine when the user enables
them (per-launch with `--video-filter=`, in the GUI, or in `vlcrc`). Chain
negotiation may create and close several candidate instances. Once one
instance accepts the stream, VLC serially pushes its decoded frames through
`Filter()` until that instance is closed.

Crucially, **`Open()` can return `VLC_EGENERIC` to opt out**. VLC then
behaves as if the filter wasn't loaded for that stream, with zero per-frame
overhead. In AUTO, any source already at or above the configured `skip-above`
height (default 720) opts out. Explicit presets bypass that gate but still
obey the no-downscale and 4×-ratio rules.

```
              ┌───────────────────────┐
              │  decoded frame from   │
              │  the codec module     │
              └──────────┬────────────┘
                         │
                         ▼
              ┌───────────────────────┐
              │  AutoUpscale Open()   │
              │  decided to engage?   │
              └────┬─────────────┬────┘
                   │ no          │ yes
                   ▼             ▼
            (frame passes  ┌──────────────────────┐
             through       │ Filter(): probe,     │
             unchanged)    │ backend, optional USM│
                           └──────────┬───────────┘
                                  ▼
                         ┌────────────────┐
                         │  upscaled      │
                         │  picture_t     │
                         └────────────────┘
```

## The decision in `Open()`

`Open()` does the following, in order:

1. Reads the negotiated visible dimensions and authoritative coded-size/crop
   metadata. zimg-supported subsampled sources are even-aligned as required.
2. Reads the options needed for planning and backend choice. Unknown
   target/backend enum values normalize to AUTO; numeric quantities retain
   endpoint clamping.
3. Detects the CPUs allowed by the process affinity mask and total RAM, then
   calls the fuzzed `up_plan_upscale()` heuristic. A bypass result returns
   `VLC_EGENERIC` with no per-frame cost.
4. Rejects opaque GPU chromas, then asks `scaler_pick()` for zimg or swscale.
5. Reads the threading, stripe, pinning, and zero-copy options, then opens the
   selected backend. In AUTO only, a zimg open failure immediately tries
   swscale once; forced selections remain strict. zimg open is cheap:
   it validates the runtime API/chroma, captures topology, computes the initial
   grid, and allocates only its private descriptor. swscale creates its
   `SwsContext` eagerly.
6. Creates an optional small USM-pool descriptor (only for nonzero USM on a
   readable Y plane; allocation failure disables sharpening), initializes
   probe/performance state, and publishes an uncropped, zero-offset target in
   `fmt_out`.

The chroma never changes. We only resize.

## Hardware-accelerated decode

When VLC uses hardware video decode (VA-API or VDPAU on the supported
Linux x86-64 target), the decoder produces **opaque GPU surfaces**: a
fourcc placeholder like `VAOP` or `VDV0` that points to a hardware
buffer, not a CPU pixel layout. Filters that need to read pixels (us,
postproc, deinterlace, etc.) cannot consume these directly.

`chroma_classify.h` exports `up_chroma_is_opaque()` which returns true
for the known opaque chroma fourccs. `Open()` calls this very early
and returns `VLC_EGENERIC` if it matches, prompting VLC to insert a
hardware-to-software download converter upstream and re-probe us with
the resolved software chroma (typically I420).

### Lazy worker initialization

Even with the opaque-chroma reject in place, VLC's filter-chain solver
may still instantiate us multiple times during chain setup when other
filters in the chain (notably `postproc`) also reject the hardware
chroma. Each instantiation is a complete `Open()` / `Close()` round
trip. Spawning the full worker pool and allocating frame scratch in every
speculative `Open()` would waste threads and memory.

To avoid this, `zimg_open()` does only the cheap work — chroma
validation, geometry computation, thread-count decision, priv struct
allocation — and returns. The actual worker pool, scratch buffers, and
per-cell filter graphs are constructed lazily on the first valid `Filter()`
picture (`zimg_lazy_init()`). Probes that don't produce a frame cost
essentially nothing. The first valid frame after a successful chain
construction pays a one-time setup cost (a few ms per worker, dominated
by `pthread_create`); subsequent frames are unaffected.

A `lazy_init_failed` sticky flag prevents retry-allocating every frame
if the first lazy init fails (e.g. because of memory pressure).

### The recursion warning is partially out of our hands

If you see `chain filter error: Too high level of recursion (3)` in the
log when combining `--video-filter='postproc:autoupscale'` with hardware
decode, that comes from VLC's chain solver hitting its hard-coded depth
limit while threading converters around two filters that both reject
the hardware chroma. We cannot raise that limit from a plugin. Lazy
init prevents the *cost* of the failed probes (no wasted threads or
allocations), but the warnings themselves still appear.

The clean workaround is to disable hardware decode for that VLC
session: `--avcodec-hw=none`. The user pays the cost of software
decoding but the chain solver finds a working configuration on its
first attempt.

The list of opaque fourccs is unit-tested (`test_chroma_classify.c`)
including a cross-predicate invariant that no opaque chroma is also
flagged as having a luma plane (which would let USM run on a GPU
surface — a guaranteed crash).

## The auto-target heuristic

The `--autoupscale-target` option accepts seven values, controlling the
output resolution height:

| Value | Constant         | Height | Notes                                           |
|-------|------------------|--------|-------------------------------------------------|
| 0     | `UP_TARGET_AUTO` | 720 or 1080 | Default. Picks based on HW capacity. Never above 1080p. |
| 1     | `UP_TARGET_720P` | 720    | Force 720p, even on capable hardware            |
| 2     | `UP_TARGET_1080P`| 1080   | Force 1080p                                     |
| 3     | `UP_TARGET_1440P`| 1440   | Explicit opt-in only                            |
| 4     | `UP_TARGET_4K`   | 2160   | Explicit opt-in only                            |
| 5     | `UP_TARGET_5K`   | 2880   | Explicit opt-in only                            |
| 6     | `UP_TARGET_8K`   | 4320   | Explicit opt-in only                            |

**The ratio cap (`UP_MAX_RATIO = 4`) binds for every preset.** Output
height never exceeds `4 × src_h`. So `target=6` (8K) from 1080p input
produces exactly 4320p, but `target=6` from 720p input only reaches
2880p (= 720×4); the user gets the best the ratio cap allows rather
than a refusal. Non-AI scalers (Lanczos, Spline36 included) don't
recover detail — they reconstruct missing pixels by interpolation, and
a 5×+ upscale produces a softer, ringy result that's worse than just
letting the player scale on output. 4× is a generous ceiling.

**AUTO never picks above 1080p, regardless of hardware.** Going higher
than 1080p doubles or quadruples the per-frame work, and most users
don't notice on typical 1080p–1440p displays. So the auto path is
conservative — it caps where the cost/benefit is well-understood, and
defers to the user for anything bigger. This is enforced by both a
unit test (`test_auto_never_above_1080p`) and a fuzzer invariant.

**`--autoupscale-skip-above` only applies to AUTO.** The default
`skip_above=720` means AUTO does not engage on 720p+ sources (where
upscaling is a debatable improvement). But explicit presets bypass
this gate — if the user explicitly requests `target=4` (4K), a 1080p
source is upscaled to 4K even though it's above `skip_above`. The
user has stated their intent; the plugin respects it.

When AUTO is active, three signals decide between 720p and 1080p:

| Signal              | Threshold for 1080p       |
|---------------------|---------------------------|
| CPU cores           | ≥ 4                       |
| Total RAM (Linux)   | ≥ 2 GB, or unknown        |
| Upscale ratio       | ≤ 4× (i.e. `src_h * 4 ≥ 1080`) |

If any threshold is missed, AUTO falls back to 720p. RAM detection uses
Linux's `sysinfo()`. Linux x86-64 is the supported deployment target.

**Forward-compatibility:** unknown preset values (e.g. a future option
introduced by a different build, or a typo'd integer outside the 0–6
range) fall through to the AUTO branch. This is asserted by
`test_unknown_preset_treated_as_auto`. The plugin should never produce
nonsense output even if some downstream tool sets `target=999`.

## The actual scaling, in `Filter()`

`Filter()` is called for every decoded frame. It:

1. While the content window is active, builds a validated crop-aware input
   view and observes luma metrics.
2. Allocates an output `picture_t` at the negotiated target dimensions.
3. Calls the active backend. Both backends build shared picture views that
   validate chroma, plane count, coded/visible crop bounds, pointer/pitch/row
   extents, and pixel pitch. The source origin comes from negotiated
   `fmt_in`, because VLC 3 clears crop offsets on allocated `picture_t`s.
   YV12 stays physical Y/V/U in VLC and is mapped to semantic Y/U/V at each
   library boundary.
4. A transient geometry/library failure drops only the current frame. A fatal
   zimg failure in AUTO closes zimg and opens swscale once for later frames;
   the triggering frame remains dropped. Forced zimg never falls back.
5. On success, optionally applies the lazy threaded USM pool in place to the
   validated output luma view, copies metadata, and releases the input.

After lazy initialization, `filter_NewPicture()` is the only routine
per-frame output-buffer acquisition; its owner may allocate or recycle that
buffer. The first valid zimg frame can allocate graphs, workers, and needed
scratch; the first USM apply can allocate its workers and private rolling
buffers. Algorithm selection is baked into the active backend.

The swscale flags include `SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT |
SWS_FULL_CHR_H_INP` on top of the chosen algorithm. These enable proper
rounding instead of fast truncation, and full-resolution chroma
interpolation. They favor output quality over the fastest swscale path.

## USM post-pass

Lanczos is the cleanest of the three swscale algorithms but produces a
slightly soft image — a known property of any linear resampler with no
explicit edge enhancement. The classical fix is **unsharp mask**:

```
out = src + amount * (src - blur(src))
```

We use a 3×3 separable Gaussian `[1 2 1]/4 ⊗ [1 2 1]/4` for the blur,
applied only to luma. `src/usm.h` retains a simple two-pass, header-only
implementation as the test oracle. Production calls `up_usm_pool_apply()`:
each worker fuses horizontal blur and vertical combine into one rolling-buffer
sweep, so there is no full-frame USM workspace or mid-frame phase barrier.

`amount` is in Q8 fixed point so the inner loop has no floating-point
ops. The user-facing option `--autoupscale-usm` is a percentage; 20%
(the default) is `amount_q8 = 51`, 100% is `256`, 200% is `512`.

**Why luma only.** Sharpening the chroma planes amplifies noise in the
colour difference signal, which shows up as colour fringing on high-
contrast edges — exactly what we don't want. Sharpening packed RGB
channels independently has the same problem, so RGB chromas bypass USM
entirely. The chroma classification happens at `Open()` via a small
`ChromaHasYPlane()` helper.

**In-place safety.** `Filter()` passes the same luma base and stride as source
and destination. Before broadcasting, the main thread snapshots each stripe's
two cross-stripe halo rows; workers then write only their owned rows. This
prevents one worker from overwriting a neighbor's future input. Exact in-place
aliasing is supported; same-base/different-stride calls are rejected, while
other partial overlap is outside the API contract. Pool output is checked
byte-for-byte against the reference path.

**Performance.** Production uses one fused, memory-bound worker sweep over the
Y plane. Throughput depends on frame geometry, compiler, CPU, worker count, and
system load, so it is measured locally rather than duplicated here. An
identity amount takes the copy fast path with no worker activity.

**Per-deployment tunables.** `up_usm_pool_create()` takes a
`stripe_min_rows` argument (`0` = compile-time default `8`) wired to
`--autoupscale-usm-stripe-min-rows`. The zimg backend reads
`scaler_ctx_t.zimg.min_stripe_lines` (`0` = default `16`) wired to
`--autoupscale-zimg-stripe-lines`. Lower values let more workers fit
on low-resolution frames at the cost of dispatch overhead; higher
values reduce worker count and dispatch overhead. Defaults match the
historical hardcoded constants and are right for almost everyone.

### Threaded USM (`src/usm_pool.h`, `src/usm_pool.c`)

The single-threaded reference (`up_usm_apply_plane` in `usm.h`) processes
the luma plane in two passes: a horizontal blur that fills a workspace
buffer, then a combine that reads three consecutive blurred rows plus the
source row to produce each output row. The threaded pool computes the same
result but **fuses** the two passes into one per-worker rolling-buffer
sweep (PERF-2, commit cc71221) — no shared workspace, no mid-frame barrier.

**Partition.** N workers, height H. Worker i owns rows
`[i·H/N, (i+1)·H/N)` (last worker absorbs rounding remainder). N is
clamped to `H / stripe_min_rows`; the default minimum is 8. Smaller configured
stripes admit more workers but increase dispatch and boundary overhead.

**Fused sweep.** Each worker keeps three rolling blur rows plus two saved
cross-stripe halo rows (five row-width buffers total). It primes the sweep with
the two rows above and at `y_start`, then for each `y` in its stripe: hblur
the next row into `dn`, combine `up`/`mid`/`dn` with `src[y]` into `dst[y]`,
and rotate the three pointers. Vertical edges clamp to `[0, H-1]`. The
worker writes only its own `dst` rows; halo snapshots preserve the original
source for an in-place call.

**Dispatch.** The main thread publishes per-frame state, snapshots halos,
arms an atomic `pending` count, increments a generation under a mutex, and
wakes the pool with one `pthread_cond_broadcast`. The last worker to decrement
`pending` signals a dedicated completion condition; the main thread waits once
against a monotonic deadline. A wait failure drains and joins the dispatched
pool before returning a sticky failure. The deadline bounds only the condition
wait: retirement remains synchronous and may wait for an already-running USM
callback so its buffers are never released while still in use.

**Lazy init.** Like the zimg backend, the USM pool's worker spawn and
private scratch allocation happen on the first `apply()` call rather than
in `up_usm_pool_create()`. A probing-only Open/Close cycle allocates and frees
only the small descriptor, so it costs almost nothing for USM.

**Identity fast path.** `up_usm_pool_apply()` with `amount_q8 == 0`
short-circuits to a memcpy (or no-op when src and dst alias) with no
thread activity and no scratch allocation. The plugin does not create or call
the pool at all when configured with `--autoupscale-usm=0`.

**Auto-skip on grainy sources.** Independent of `amount`, the plugin
also bypasses USM after the content probe completes (60 valid luma views) when
the source's mean squared Laplacian response exceeds
`p_sys->usm_sharp_threshold` (set from
`--autoupscale-usm-sharp-threshold`, default `3500`). On heavily
textured / grainy content USM amplifies the noise without adding
perceived sharpness, so the plugin sets a `usm_skip_sharp` flag in
`filter_sys_t` and `ApplyUsmIfEnabled()` returns early thereafter.
Set the option to `0` to disable the feature entirely (USM keeps
running regardless of source).

**Optional flat-skip (compile-time).** The pool also has an opt-in
per-stripe early-out (`-DUSM_POOL_FLAT_SKIP=1` at compile time): before
the sweep, each worker samples its stripe's middle row's horizontal
activity, and if it is below `USM_FLAT_AVG_DELTA` (≈ 2/255 average
neighbour difference) the worker identity-copies its whole stripe instead
of running the blur+combine sweep. Defaults to off because the implicit
byte-identity guarantee against the single-threaded reference is dropped
(per-pixel delta of up to a few LSB on borderline-flat content). Useful
for benchmarking and for content-specific builds where letterbox /
fade-to-black dominate.

**Correctness verification.** The crucial invariant is that pool
output equals single-threaded output bit-for-bit. This is enforced by
`tests/test_usm_pool.c`, which runs `up_usm_apply_plane` and
`up_usm_pool_apply` on synthetic data and compares every byte. The
test exercises different worker counts, unusual aspect ratios, odd dimensions,
geometry clamps, and repeated frames that verify lazy initialization.
End-to-end live VLC byte-identity (decoded video MD5
matches the pre-threaded-USM build) is the integration check.

**Why bit-identity rather than approximate.** The math is fully
deterministic — same inputs produce same outputs, regardless of which
thread runs which row. There's no floating-point reordering involved
(USM operates on uint8 with int arithmetic). If the threaded variant
produced different output, that would mean either a partition bug
(rows missed or processed twice) or a race (workspace read before
write). Both should fail the test.

## Composing with VLC's `postproc` filter

For low-bitrate sources (DVD rips, old web video, anything with visible
blocking or ringing), VLC's built-in `postproc` filter — which uses
libpostproc for deblocking and deringing — combines very well with this
one. Apply postproc *first* so the cleaner picture goes into the upscaler:

```sh
vlc --video-filter='postproc:autoupscale' --postproc-q=6 lowres.mp4
```

The order matters. `postproc:autoupscale` deblocks, *then* upscales,
*then* sharpens. Reversing the order would amplify compression
artifacts before cleaning them up.

## Why two backends instead of one

### The interface

The plugin doesn't call libswscale or libzimg directly. Instead `Open()`
asks `scaler_pick()` for a backend that can handle the current
`(chroma, algo)` pair, gets back a `const scaler_backend_t *` (a struct
of function pointers), and treats it opaquely from then on:

```c
struct scaler_backend_s {
    const char *name;
    int id;
    int  (*supports)(vlc_fourcc_t chroma, int algo);
    int  (*open)   (scaler_ctx_t *);
    scaler_process_status_t
         (*process)(scaler_ctx_t *, const picture_t *src,
                    const picture_t *dst);
    void (*close)  (scaler_ctx_t *);
};
```

Two implementations exist: `scaler_swscale.c` and `scaler_zimg.c`.
Either backend is free to decline a chroma it can't handle (zimg
declines NV12/NV21 and packed RGB; swscale covers all nine supported
CPU-readable chromas). AUTO has three fallback seams: `scaler_pick()`
uses swscale on a zimg `supports()` miss, `Open()` retries swscale after
a zimg open failure, and `Filter()` switches once after a fatal zimg
processing failure. A transient processing failure drops only that frame.
Forced backends are strict and never fall back.

### The runtime trade-off

| Aspect            | swscale                              | zimg                                          |
|-------------------|--------------------------------------|-----------------------------------------------|
| Spline36          | no — falls back to Lanczos           | yes (native, recommended for upscaling)       |
| Chroma filter     | shared with luma                     | separately tunable (we pick the same one)     |
| Rounding          | accurate via `SWS_ACCURATE_RND` flag | accurate by default                           |
| NV12 / NV21       | yes                                  | no — needs depack/repack stage we don't add   |
| Packed RGB        | yes                                  | no                                            |
| Threading         | single-threaded per frame            | row×column worker grid via a persistent worker pool |
| Build dependency  | always present (VLC links it)        | optional — `pkg-config zimg`                  |

zimg's two wins versus swscale:

1. **Spline36** — noticeably sharper edges than Lanczos with less
   ringing on text and high-contrast detail. The recommended filter
   for upscaling and the plugin's default.
2. **Grid threading.** zimg's image-buffer model supports independent
   sub-images. The normal path splits the destination into horizontal
   stripes; very wide/short frames add column tiles so every useful worker
   gets a cell. Each cell has its own graph and temporary buffer. See the
   "Threading" section for the partition and seam rules.

### Why not just always use zimg

Three reasons:

1. **Chroma coverage.** zimg expects fully-planar YUV. NV12/NV21 are
   semi-planar (interleaved UV), and converting them just to feed zimg
   would eat any quality advantage. Packed RGB is even worse. Letting
   swscale handle these is cleaner than building a depack/repack
   adapter.
2. **Distro footprint.** Some users will build the plugin on systems
   without `libzimg-dev`. The Makefile detects that via `pkg-config
   --exists zimg` and silently omits the zimg backend if missing — the
   plugin still works, just always uses swscale.
3. **Escape hatch.** Bugs happen. Having `--autoupscale-backend=2` to
   force swscale gives users a one-flag workaround if zimg behaves
   oddly on a particular file or hardware.

## Why not something even fancier

The two non-options for shipping in this plugin:

- **AI upscalers** (Real-ESRGAN, FSRCNN, EDSR). Far higher quality, but
  would need to ship model weights, depend on a runtime (ONNX Runtime,
  ncnn, etc.), and even on a discrete GPU they're around the edge of
  realtime for 1080p output. Not viable as a small, self-contained C
  module.
- **GPU shaders** (Anime4K, RAVU, FSRCNN-GLSL via Vulkan compute). Far
  higher quality, much faster than CPU. But you need an output-stage /
  `glconv` module rather than a `video filter` — different VLC plumbing,
  and your filter would be a different project. mpv with the Anime4K
  user-shaders is a better fit if that's your goal.

So zimg with Spline36 (threaded across a row×column worker grid) is the
realistic ceiling for this plugin's architecture.

## Content-aware quality probe

Not every source benefits from upscaling. A heavily compressed stream can
contain visible block artifacts that become larger and more distracting after
resampling. For these "lost cause" sources, the
honest answer is "don't upscale" — but how do we detect them
automatically?

### What's measurable, what isn't

Real per-frame quality measurement (PSNR, SSIM, VMAF) requires a
*reference* image to compare against. We have only the source frame
and our upscaled output; the *true* high-resolution image that the
source was downscaled from doesn't exist in the input stream. So we
can't say "our upscaled 1080p is closer to ground truth than display-
time bilinear." There's no ground truth to be closer to.

What IS measurable cheaply, on the source itself, are two
no-reference proxies:

1. **Mean squared Laplacian response** on the luma plane — a
   high-frequency-energy estimate. High value means the source has edges,
   texture, or noise. Low value means the source is soft — heavily blurred,
   noise-reduced, or just blank — and there is little for a non-AI scaler
   to interpolate. Despite the historical helper name
   `up_laplacian_variance`, this is energy, not statistical variance.

2. **Block-edge intensity** at 8-pixel grid boundaries. H.264 and
   H.265 both use 8×8 transform blocks; when bitrate is too low, the
   reconstruction has visible discontinuities at those boundaries.
   High block-edge intensity means heavy compression — and upscaling
   amplifies blocking, making the artifacts more visible.

The decision rule combines the two: bypass-advisory fires only when
the source is BOTH very soft AND very blocky. Soft-but-not-blocky is
fine (smooth output is acceptable). Blocky-but-detailed is fine (the
user wants the detail upscaled). Only soft+blocky is the case where
upscaling strictly hurts.

### What's actually achievable in VLC's filter API

VLC 3's video-filter API doesn't allow runtime format renegotiation.
The filter declares its `fmt_out` in `Open()` and the downstream
chain locks in expecting that format from frame 1. We physically
can't start emitting input-resolution pictures mid-stream — every
downstream stage is wired to receive 1080p (or whatever target was
chosen).

So the probe is **diagnostic only**: when it detects soft+blocky
content, it logs a one-time advisory recommending the user disable
AutoUpscale (or set `--autoupscale-target=1`) for next playback. The
filter keeps producing target-resolution output to satisfy the
contract it made when the chain was built.

### Implementation

`src/content_probe.h` is a header-only module. Its production operations are:

- `up_laplacian_variance(plane, stride, w, h, *n_out)` — samples every
  fourth row and column across the full visible luma plane and accumulates
  squared Laplacian responses.

- `up_block_edge_strength(plane, stride, w, h, *n_out)` — measures
  absolute pixel differences across `UP_PROBE_BLOCK_SIZE` boundaries using
  the `UP_PROBE_GRID_STEP` sampling step.

- `up_probe_observe(*accum, ...)` — accumulates a valid picture view's
  metrics. A zero metric still advances the observation count.

- `up_should_bypass_for_content(*accum)` — computes the advisory verdict from
  the configured detail, block-edge, frame, and sample thresholds.

- `up_should_skip_usm_for_sharpness(*accum, threshold)` — independently
  gates the USM post-pass when the mean squared response is above the
  configured threshold.

The probe closes after the fixed `UP_PROBE_WINDOW_FRAMES` successfully
validated luma-picture observations. Invalid views are skipped rather than
consuming the window. The fixed sample step spans the full visible plane, so cost scales with
source dimensions and is paid only during that startup window.

### Honest scope

The probe is a heuristic, not a quality measurement. It catches the
worst case — heavily compressed soft sources where upscaling
strictly hurts — and lets everything else through. Real-world
"would the upscaled picture look better than the original on the
user's display" depends on display size, viewing distance, and
personal preference; no in-filter algorithm can answer that.

The named thresholds in `src/content_probe.h` are deliberately conservative:
the advisory requires both low detail energy and elevated block-edge
intensity, so either condition alone is insufficient.

The same accumulator drives a second decision. When `lap_mean` exceeds
`p_sys->usm_sharp_threshold`, the plugin sets `filter_sys_t.usm_skip_sharp`, and every
subsequent `ApplyUsmIfEnabled()` returns early. The cutoff and the
master switch are merged into a single VLC option,
`--autoupscale-usm-sharp-threshold`: any positive value enables the
feature with that cutoff; `0` disables it.

### Verification

The diagnostic verdict is observe-only: metric helpers accept `const` input
and the advisory never changes scaling. `test_content_probe.c` exercises flat,
checkerboard, gradient, and block-grid planes; invalid geometry; accumulation;
advisory boundaries; and the
independent USM threshold. `fuzz_content_probe.c` drives the pointer arithmetic
under ASan/UBSan and checks that the input remains unchanged. Disabling
`content-probe` suppresses only the advisory; collection can continue when the
USM sharpness gate needs the same metric.

## Performance auto-tuning

### Compiler choice and SIMD

The USM kernels (`up_usm__hblur_row`, `up_usm__combine_row` in
`src/usm.h`) are autovectorization-friendly inner loops. Non-aliasing scratch
relationships use `restrict`; source and destination rows retain the aliasing
contract required for in-place operation. Production compiles the whole
`usm_pool.c` translation unit with the optimization level selected in the
Makefile, without per-function optimization pragmas.

SIMD width is controlled by `MARCH`. Wider variants process more lanes per
instruction, but real throughput remains CPU-, compiler-, and
memory-bandwidth-specific. Use the benchmark tooling for same-host comparison;
checked-in timing tables are intentionally avoided.

**Default build: single-baseline at `-march=native`.** The plugin
ships as a source distribution — every user builds it on the same
machine they run it on — so `MARCH=native` and `MULTIVERSION=0` are
the right defaults. The compiler tunes for the local CPU (znver4 or
Skylake-X scheduling, plus extra ISA bits like VBMI2 / BF16 / GFNI /
VAES on top of the v4 baseline) and the binary contains exactly one
copy of `usm_pool.c`. No runtime dispatcher, no dead-code variants.
The engagement log prints `simd=default` to indicate the
single-baseline build:

```
AutoUpscale engaged: <source> -> <target> (... simd=default)
```

**Multi-ISA build: `make MULTIVERSION=1`.** When you need one Linux x86-64
binary across several CPU classes (for example distro packaging),
`MULTIVERSION=1` compiles `usm_pool.c` three times at three baselines
(SSE2 / AVX2 / AVX-512) into three separate `.o` files with renamed
public symbols. A thin dispatcher in `src/usm_pool_dispatch.c` runs at
`.so` load time via `__attribute__((constructor))`, calls the shared
full-level probes, and points four function pointers
(`up_usm_pool_create/destroy/apply/effective_threads`) at the
highest-supported variant:

```c
if (up_cpu_supports_v4())
    /* point at *_avx512 entry points */
else if (up_cpu_supports_v3())
    /* point at *_avx2 entry points */
else
    /* point at *_sse2 entry points */
```

Combine `MULTIVERSION=1` with an appropriate Linux x86-64 ISA baseline
(for example `x86-64-v3` for AVX2 or `x86-64` for SSE2) so the rest of
the plugin (autoupscale.c, scaler*.c) uses a deployment-compatible baseline.
The dispatch happens **once** at `dlopen`
(before VLC's plugin scanner or `vlc-cache-gen` calls into the .so), so
per-frame forwarding is just one indirect call. The chosen variant name is
exported as `up_usm_pool_variant_name` and
printed in the engagement log: `AutoUpscale engaged: ... simd=avx512`.

**Why three .o files instead of `__attribute__((target_clones))`?**
`target_clones` requires non-static linkage on the cloned function,
which would defeat the `static inline` of the hot kernels in `usm.h`.
By compiling the entire usm_pool.c TU at three -march levels, each
variant gets to inline its kernels at its own SIMD width. The tradeoff is
additional binary size and build complexity.

**ISA guard limitation:** the dispatcher uses `up_cpu_supports_v3()` and
`up_cpu_supports_v4()`. Supported compilers probe the complete levels directly.
The compatibility fallback for older compilers checks a feature conjunction
that does not enumerate every inherited feature, so deploy those builds only
where the chosen variant's full ISA level is known to be available.

Decoded MD5 is **byte-identical** across SSE2, AVX2, AVX-512 builds
AND across MULTIVERSION=0/1 — the kernels do bytewise saturating
arithmetic, and SIMD just runs the same arithmetic in more lanes
per instruction. The exact pixel-level output you'd get from a
scalar reference build is preserved across all SIMD widths and
both build modes.

The plugin ships with the highest-quality defaults available (zimg +
Spline36 + USM 20%) and watches its own per-frame processing time so it
can tell the user when those defaults are too expensive for their
hardware.

### What's measured

Inside `Filter()`, a `clock_gettime(CLOCK_MONOTONIC)` reading is taken
just before `scaler->process()` and just after the USM post-pass. The
delta is the time the plugin itself spent on this frame — independent
of decode, downstream filters, encode, or display. The figure is fed
into `up_perfmon_record_ns()` from `src/perfmon.h`.

### EWMA, not raw samples

Per-frame timings on a real machine are noisy: a context switch or cache miss
can make an isolated sample unrepresentative. Reacting to one sample would
produce false alarms on busy machines.

Instead, perfmon keeps an exponentially-weighted moving average:

```
ewma <- ewma + (sample - ewma) >> 3
```

The right-shift-by-3 means α = 1/8, giving a half-life of about 5.5
samples. Sustained overruns rapidly converge the EWMA toward the true
mean; brief spikes barely move it. The "brief spike doesn't trigger"
behavior is one of the unit tests.

### Warmup and minimum-samples

The warmup and minimum-sample thresholds in `src/perfmon.h` suppress noisy
early measurements. Warmup samples are discarded because they include cold
caches, codec startup, and scaler table construction. The EWMA is then allowed
to settle before it is compared with the frame budget.

After both thresholds are satisfied, the EWMA is checked once per frame
against `1 / target_fps`. The first time it exceeds the budget,
`up_perfmon_record_ns()` returns 1 — exactly once. After that the
`has_warned` field is latched and subsequent calls return 0 even if the
overrun continues. No log spam.

### Why `msg_Info`, not `msg_Warn`

VLC 3.0's default log verbosity counterintuitively shows level-0
`msg_Info` but suppresses level-2 `msg_Warn`. Most users never pass
`--verbose=1`, so `msg_Warn` would be invisible by default — exactly
the opposite of what we want for a hint that says "your settings might
be too aggressive". The hint is therefore emitted as `msg_Info` with a
`"Performance warning:"` prefix in the message body. Visible at default
verbosity, doesn't pollute error logs, intent unambiguous from the text.

### Suggestions order

The follow-up `msg_Info` lines list tuning options from least to most
quality cost:

1. Drop Spline36 → Lanczos (algo=2). Same backend, almost-same look,
   measurably faster.
2. Drop Lanczos → Bicubic (algo=1). Noticeable quality drop, big speed
   win.
3. Disable USM (usm=0). Tiny CPU savings, slightly softer output.
4. Force swscale (backend=2). Different scaler entirely; faster
   single-threaded path.
5. Force a lower target. Fewer output pixels reduce scaler and USM work.

Plus the escape hatch: `--autoupscale-target-fps=0` to silence the hint.

### What it does NOT do

- **Does not auto-tune.** The plugin doesn't change its own settings
  mid-stream. Re-opening the scaler graph for a new algo would require
  unwinding the picture pool and re-negotiating the format with VLC,
  and getting that wrong is the kind of thing that crashes the player.
  Auto-tuning is on the roadmap; the warning is the safe first step.
- **Does not measure decode/encode cost.** Only the plugin's own
  `process` + `usm` work. If your CPU is saturated by the decoder, this
  hint won't fire and the suggested changes won't help.
- **Does not measure across runs.** Each new stream gets a fresh perfmon and
  can emit its own hint.

## Threading

The zimg backend grid-threads each frame. Normal frames use horizontal
stripes; very wide/short frames also split columns so the useful worker
budget is not capped by output height. Default worker preference =
`cores/2 − 2`, clamped to `[1, 64]`. `cores` comes from the
calling thread's Linux affinity mask, so taskset and cgroup/cpuset limits
are honored. The "/ 2" reserves half the available CPUs for VLC's main
thread, decoder, encoder, audio, vout, and other libraries VLC pulls in;
the "− 2" is an extra absolute reserve. Users whose workload needs a
different balance can override with
`--autoupscale-threads=N`. The topology and decision logic live in
`src/threading.h` and are exercised by `tests/test_threading.c`; the same
topology snapshot supplies exact sparse CPU IDs when worker pinning is on.

### Architecture

`Open()` is deliberately cheap — it validates the chroma, computes an initial
worker grid, and saves parameters, so VLC's chain-solver probing does not pay
for thread spawns or megabytes of scratch. The first valid picture is checked
against zimg's 32-byte direct-address and stride requirements. An unaligned
side switches to aligned scratch before graphs exist; if source copy-in is
needed, a column grid is recomputed as rows-only because column graphs require
full-width direct source reads. The backend then lazily:

1. Keeps the initial grid computed in `Open()` unless an unaligned source
   requires copy-in, in which case it recomputes a rows-only grid. For normal
   frames `N_COLS == 1`.
2. Allocates aligned plane-I/O scratch only for a side that copies. With the
   default both-sides zero-copy there is no shared plane scratch; each graph
   still owns its required zimg temporary buffer, and column cells own a small
   destination-tile scratch.
3. Builds one `zimg_filter_graph` per grid cell, configured for that cell's
   sub-image — stripe height, and for a column tile a full-width source with
   an `active_region` cropping its column window (so zimg reads cross-
   boundary source context). Independent column graphs can still restart
   resize phase and produce bounded seam deltas. Dimensions align to 2 for
   chroma subsampling.
4. Spawns the worker threads, which block on a shared condition variable.

If a completion-barrier wait fails, the pool is marked fatal, any dispatched
generation is drained, and every worker is joined before the frame returns to
its owner. The deadline bounds the completion wait, not that safe retirement:
an already-running zimg graph callback is allowed to finish before join returns.
Later calls fail without touching the picture.

At each `Filter()` call the backend first builds shared, crop-aware
`picture_view` objects. They validate chroma, plane layout, pointers, pixel
pitch, row extents, and negotiated source crop before any worker sees a
pointer. Invalid geometry is transient and drops that frame. Later storage
drift that violates an already-built direct graph's 32-byte contract is also
transient. For a safe view the backend:

1. **Copy-in** (when selected explicitly with
   `--autoupscale-zerocopy-src=0` or forced by first-frame alignment): each
   worker `memcpy`s its source stripe into scratch. Otherwise graphs read
   VLC's source picture directly.
2. Points each worker at the current frame's planes, bumps a shared
   "generation" counter under a mutex, and wakes all workers with ONE
   `pthread_cond_broadcast` (SCAL-2 — was one `sem_post` per worker).
3. Waits on one dedicated completion condition: each worker decrements an atomic
   `pending` counter after its cell and the one that drives it to zero signals
   the condition. Spurious wakes recheck `pending` against the same monotonic
   deadline.
4. **Copy-out** (when selected explicitly with
   `--autoupscale-zerocopy-dst=0`, forced by first-frame alignment, or required
   for column tiles): each worker `memcpy`s its result into VLC's output
   picture. Otherwise a non-tiled graph writes VLC's picture directly.

At `Close()` the backend wakes every worker to exit (one broadcast), joins
them, and frees.

### Why copy-in / copy-out

Because the alternative — passing VLC's pool-managed picture buffers
directly to per-stripe zimg graphs from worker threads — is unreliable.
We isolated the failure with a layered diagnostic:

- A **standalone reproducer** (`tools/zimg_stripe_repro_threaded.c` in
  the earlier lineage) exercises the per-stripe threading pattern with fresh
  `aligned_alloc`'d buffers and passes under valgrind and ASan.
- An **in-VLC self-test** (running the same logic from the plugin's
  Open() at startup) also works perfectly inside VLC's process address
  space.
- The same pattern using **VLC's `picture_t->p[i].p_pixels` pointers**
  segfaults inside `zimg_filter_graph_process` on a worker call, even with a
  mutex serializing the calls (so it is not a
  threading-safety issue between the workers — it's something specific
  to those buffers).

We didn't fully isolate the root cause inside VLC's picture allocator
(the segfault was downstream of any logging we could add, and valgrind
couldn't reach it inside VLC's process before timing out on its own
overhead). So the original design copied **both** sides through scratch and
paid extra memory bandwidth for guaranteed correctness.
That caution has since been relaxed on both sides — see below — behind shared
picture-view validation and direct-I/O alignment checks, but either side can
still be forced back to the copy path with its
`--autoupscale-zerocopy-*=0` option.

### Zero-copy on both sides

The crash investigation specifically implicated source buffers from the
upstream picture pool. Destination zero-copy was enabled first after direct
tests; correctness does not assume that `filter_NewPicture()` returns a fresh,
non-recycled allocation:

- Workers receive VLC's destination picture pointers (with VLC's pitch)
  per-frame instead of priv scratch pointers.
- `alloc_scratch_buffers()` skips the destination-plane allocation.
- Graphs don't bake in destination stride — it lives in the per-call
  `zimg_image_buffer` — so the stride can vary per frame without rebuilding.

Byte-identical decoded MD5 between `zerocopy-dst=0` and `=1` (single- and
multi-threaded) confirmed it pixel-correct, so it **defaults to 1 (on)**.
The **source** side is the symmetric twin — graphs read VLC's source
picture directly, `alloc_scratch_buffers()` skips the src allocation — and
**defaults to 1 (on)** as well, guarded by the shared crop-aware picture-view
validation and zimg alignment check described above. The one exception is a
column tile, which
always copies its result out of a private tile scratch into the destination
sub-rectangle.

`tests/test_scaler_zimg.c` holds the source/destination I/O mode combinations
byte-identical when they retain the same row-only topology, including the
single-worker path. On a wide/short frame, source copy-in disables column
tiling and therefore changes the graph
partition; any difference is governed by the seam bound below rather than by
the copy itself. Skipping copy-out avoids a frame-sized transfer, and copy-in
is a comparable transfer. The validator prevents malformed geometry and
unaligned direct I/O; it cannot
prove every VLC pool-lifetime property. If a VLC build still misbehaves, set
the offending side back to the copy path with
`--autoupscale-zerocopy-src=0` / `--autoupscale-zerocopy-dst=0`
(troubleshooting recipes in [USAGE.md](USAGE.md)).

### Stripe-boundary caveat

Each independent row or column graph can restart resize phase and introduce a
sub-pixel seam delta. Column `active_region` supplies cross-boundary source
context but does not make separate graphs phase-identical. The standard zimg
integration suite and randomized seam harness enforce the configured seam
contract. Synthetic high-frequency inputs make the effect more visible. A
user sensitive to these seams can set
`--autoupscale-threads=1`.

## Why decision logic is in a header

`src/upscale_logic.h` is intentionally header-only and depends on neither
VLC nor FFmpeg. The plugin includes it; the tests include it; the fuzzer
includes it. The same code runs in all three places, which means:

- Tests catch logic bugs without needing VLC installed.
- The fuzzer exercises the decision functions without VLC dependencies.
- Distro packagers can run `make test` in their build sandbox without
  pulling in `vlc-devel` for tests.

The trade-off is that all helper functions are `static inline`. That remains
manageable for this small, dependency-free decision module.

## Testing strategy

The project has complementary layers of correctness checking, each catching a
different class of bug:

### Unit tests — exact-value assertions

`make test` runs the unit and contract executables under AddressSanitizer and
UndefinedBehaviorSanitizer. The target itself is the source of truth for the
active executable and case set. It covers resolution planning, USM and its
worker pool, performance monitoring, topology decisions, picture geometry,
chroma classification, backend selection, scaler contracts, variant
equivalence, and resource lifetime. Memory errors and signed-overflow bugs are
caught even when asserted output happens to be correct.

The `upscale_logic` suite includes targeted regression tests for design
rules that aren't otherwise verifiable from output alone:

- `test_auto_never_above_1080p`: AUTO must never produce > 1080p output
  regardless of HW capacity. Loops over realistic source heights with
  hypothetical mega-hardware (256 cores, 1 TB RAM).
- `test_unknown_preset_treated_as_auto`: unknown preset values
  (`UP_TARGET_MAX + 1`, `INT_MAX`, `INT_MIN`, `999`, `-1`) all fall
  back to AUTO. Forward-compat guard.
- `test_high_res_ratio_cap_boundary`: the ratio cap is exact, not
  off-by-one. For each high-res preset, asserts both the exact-fit
  edge case (`src_h * 4 == target`) and the one-below case
  (`(src_h-2) * 4 < target`) produce the right values.
- `test_compute_4k_aspect_preserved`, `test_compute_8k_aspect_preserved`:
  exact 16:9 alignment from 1080p source to 4K and 8K.
- `test_plan_full_ladder_spot_check`: end-to-end smoke through
  `up_plan_upscale` for every preset 0..6 with a realistic 854×480
  source, asserting evenness, no-downscale, and ratio-cap compliance.

### Smoke fuzzers — deterministic mass testing

`make fuzz-smoke` runs the standalone `tests/fuzz_*.c` harnesses under ASan
and UBSan. The Makefile and each harness define their live iteration and
boundary-sweep budgets. Every harness drives a function or small helper group
with deterministic random input and verifies explicit post-conditions.

The suite covers resolution planning, USM, performance monitoring, topology,
plane copying, stripe and tile geometry, chroma mapping, scaler opening,
content probing, picture views, and cross-SIMD byte equivalence.

`fuzz_upscale_logic` and `fuzz_frame_shape` use **biased input
selection** in their smoke mains. Without bias, raw values almost never hit
valid presets, realistic source heights, or real chroma fourccs, so structured
branches would go essentially untested. Biased cases guarantee meaningful
coverage while raw random bytes retain unstructured exploration.

### libFuzzer — coverage-guided exploration

`make fuzz` builds the clang-libFuzzer targets that explore the input space
using coverage-guided mutation. The CI workflow is the source of truth for
which targets run and for how long. The harnesses share
their invariant checker with the smoke fuzzers, so any bug found by
libFuzzer is also a violation of an explicit, named property — not
just a crash.

**Corpora.** Structured seed corpora live under `tests/corpus*`. Directory
contents and harness parsers are the source of truth for seed counts, binary
formats, and supported boundary cases. Seeds give libFuzzer a foothold in the
structured input space so mutations can cross over real chroma values,
resolutions, API boundaries, and prior regressions.

- `tests/corpus/` feeds `fuzz_upscale_logic` values parsed as `<iiiiII>`.
- `tests/corpus_frame_shape/` feeds `fuzz_frame_shape` values parsed as
  `<IIiiii>`.
- `tests/corpus_scaler_chroma/` feeds `fuzz_scaler_chroma` values parsed as
  `<4s B 3x>`.
- `tests/corpus_usm_variants/` feeds `fuzz_usm_variants` values parsed as
  `<HHhBB>`.

### Concurrency stress — TSan-instrumented worker dispatch

`make stress` exercises the `usm_pool` worker dispatch under both
ASan + UBSan and ThreadSanitizer (TSan). The threading is the most
likely place for the project to hide a bug that compiles cleanly,
passes unit tests, and *occasionally* produces wrong output — exactly
the kind of bug TSan catches even when the pixels happen to match on
this run.

`tests/stress_usm_pool.c` defines the live stress matrix. It spans worker-count
clamping, common video shapes, identity and nonzero USM amounts, unusual aspect
ratios, odd dimensions, large frames, and the single-worker path.

Every frame's output is byte-compared against the single-threaded
reference `up_usm_apply_plane`. **Bit-perfect match is required** —
a single byte of divergence fails the run. The input data mutates
every frame (xorshift32-seeded fill) so workers must fetch fresh
pointers each call; a bug where workers cached stale source pointers
would surface as "first frame matches, subsequent frames diverge."

ThreadSanitizer instrumentation tracks every memory access and
synchronization event. A race in halo publication, per-frame pointers,
completion state, or worker results is reported even when the output happens
to match. Any TSan report or output divergence fails the run.

The CI job runs both ASan+UBSan and TSan builds on pushes and pull requests
targeting `main`/`develop`.

### Fuzzer invariants

`fuzz_upscale_logic.c` asserts these resolution-ladder design rules
on every iteration:

1. **Generic contract** — bypass returns zeroed output; non-bypass
   returns even, in-range, non-downscaled, aspect-preserving dims
   under the 4× ratio cap.
2. **AUTO ceiling** — `preset == UP_TARGET_AUTO` implies
   `out.height <= 1080`. Catches a regression where AUTO might be
   silently routed through one of the high-res branches.
3. **Known-preset target reaching** — for each of the seven valid
   presets, output height must reach the preset's nominal target (or
   the ratio cap, whichever is smaller). Catches a regression where a
   `case` falls through to AUTO and silently produces 720p instead of
   the requested 4K.

`fuzz_frame_shape.c` asserts these chroma-and-plane design rules:

1. **Chroma class mutual exclusion** — a fourcc is never both
   "opaque" and "has-y-plane". A USM operation on opaque GPU memory
   would be a use-of-uninitialized-memory at best, a segfault at
   worst.
2. **Class spot-checks** — known opaque fourccs (VAOP, VDV0, DX11,
   MMAL, CVPN) must be detected by `up_chroma_is_opaque`; known
   y-plane fourccs (I420, YV12, NV12, NV21, I422, I444) must be
   detected by `up_chroma_has_y_plane` and not flagged as opaque;
   known packed fourccs (YUY2, UYVY, RV32, RV24, BGRA) must not be
   classified as has-y-plane.
3. **Plane geometry** — pitch/lines never produce values smaller
   than `ceil(w / 2^sub_w)` or `ceil(h / 2^sub_h)`; pitch is always
   64-byte aligned; `round_up_pitch` and `round_up_lines` never
   shrink their input; `zimg_plane_idx` returns a value in [0, 2]
   regardless of swap.
4. **Stripe partition coverage** — when the caller honors the
   production constraint `n_stripes <= dst_h / UP_STRIPE_MIN_DST_LINES`
   AND `src_h / n_stripes >= 4`, the per-stripe ranges form a
   non-overlapping, contiguous cover of `[0, src_h)` and `[0, dst_h)`.
5. **Invalid-input rejection** — `compute_stripe_bounds` returns 0
   for `i < 0`, `i >= n`, `n <= 0`, `src_h <= 0`, or `dst_h <= 0`,
   without writing to its output pointers (proven by canaries).

`fuzz_scaler_chroma.c` exercises the VLC-interface boundary —
specifically `up_chroma_to_zimg()` from `src/scaler_zimg_chroma.h`,
which maps a VLC chroma fourcc to zimg's (sub_w, sub_h, yv12_swap)
triple. This is the function the production zimg backend calls on
every frame to decide whether and how to consume it. The invariants:

1. **Known-supported coverage** — every iteration verifies that
   I420 → (1,1,0), YV12 → (1,1,1), I422 → (1,0,0), I444 → (0,0,0).
   Catches a sub_w/sub_h regression instantly.
2. **Known-unsupported rejection** — NV12, NV21, YUY2, UYVY, RV32,
   RV24, BGRA, VAOP, VDV0, DX11 must all return 0. Catches a
   regression where someone naively adds NV12 to the supported list
   (would crash at runtime — zimg can't consume semi-planar UV).
3. **Random fourcc safety** — any 32-bit value must produce rc=0 or
   rc=1, never crash, never leave outputs partially written when
   rc=1 (sentinel detection on each output).
4. **NULL output rejection** — passing NULL for any subset of the
   three output pointers must return 0 cleanly without crashing.
5. **Cross-property: zimg-supported ⊂ has-y-plane** — every chroma
   `up_chroma_to_zimg` accepts must also be classified as has-y-plane
   by `chroma_classify.h`. Strict subset (NV12 has Y plane but isn't
   accepted by zimg).
6. **Cross-property: zimg-supported ∩ opaque = ∅** — no opaque
   chroma should ever be accepted. Catches a regression where an
   hwaccel fourcc accidentally got added.

Both fuzzers print the violated invariant with full input context to
stderr, flush, then `abort()`. libFuzzer reports the abort as a
finding and minimizes the input. During development this loop caught bugs in
the test harness itself, showing that the invariants are tight enough to
distinguish "wrong test" from "correct code".

### Bench tooling

`tests/bench_usm_pool.c` is a standalone perf bench separate from the
correctness tests. It accepts `<threads> <width> <height> [frames]
[amount] [fill]` and prints one CSV line of µs/frame, with
warmup-and-discard handling so worker-spawn and first-touch page
faults don't pollute the timed loop. The supported fill modes drive different
content patterns through the kernels, which is useful when measuring the
effect of compile-time toggles
like `USM_POOL_FLAT_SKIP`.

`scripts/bench_matrix.sh` defines the current `(threads × resolution)` grid
and aggregation policy. It is used to compare the pool against itself across
configurations during development.

### Coverage

`make coverage` builds a separate `build/cov/` test binary set with
`--coverage -fprofile-arcs -ftest-coverage`, runs them, then prints
per-file summaries via `scripts/coverage_report.sh` and per-function summaries
via `scripts/coverage_per_function.sh`. The scripts fail the build when a
tracked file or function falls below 80% coverage, or when gcov data is
missing or malformed. Their generated output is the source of truth for
tracked totals and percentages.

The `make coverage` step is wired into the GitHub Actions CI job
(`.github/workflows/ci.yml`) so a coverage-gate failure rejects the CI job.
The SonarCloud job additionally converts the same instrumented run into a
SonarQube generic-coverage report with `gcovr` and uploads it via
`sonar.coverageReportPaths`, so line coverage of the production sources is
visible on SonarCloud too.

### Cyclomatic complexity

`lizard` is the standard tool. `make complexity` and `make analyze`
enforce CCN ≤ 10 for every function in `src/` and `tests/`. The Makefile
invocation is the source of truth. Helpers
extracted for complexity stay `static` (or `static inline` for
header-only modules) and live in the same file as their caller.
