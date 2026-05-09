# How AutoUpscale works

This document explains what the plugin actually does at runtime, so you can
decide whether the trade-offs match what you want.

## Filter lifecycle in VLC

VLC video filters are loaded by the playback engine when the user enables
them (per-launch with `--video-filter=`, in the GUI, or in `vlcrc`). For
each video stream, VLC calls the filter's `Open()` exactly once. If `Open()`
returns success, VLC pushes every decoded frame through `Filter()` until the
stream ends, at which point `Close()` is called.

Crucially, **`Open()` can return `VLC_EGENERIC` to opt out**. VLC then
behaves as if the filter wasn't loaded for that stream, with zero per-frame
overhead. AutoUpscale uses this aggressively: any source already at or above
the configured `skip-above` height (default 720) opts out. The user can
leave the filter enabled globally without paying any cost on HD content.

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
            (frame passes  ┌──────────────┐
             through       │  Filter():   │
             unchanged)    │  sws_scale() │
                           └──────┬───────┘
                                  ▼
                         ┌────────────────┐
                         │  upscaled      │
                         │  picture_t     │
                         └────────────────┘
```

## The decision in `Open()`

`Open()` does the following, in order:

1. Reads `i_visible_width` / `i_visible_height` from the input format
   (falls back to `i_width` / `i_height` if visible isn't set).
2. Reads the module options (`target`, `algo`, `skip-above`, `usm`,
   `backend`, `target-fps`, `threads`, `zerocopy-dst`, `content-probe`,
   `usm-stripe-min-rows`, `zimg-stripe-lines`, `usm-skip-sharp`,
   `usm-sharp-threshold`).
3. Calls `DetectHardware()` to get core count and total RAM.
4. Calls `up_plan_upscale()` from `upscale_logic.h` to decide whether to
   engage and, if so, what target dimensions to use. This is where all the
   heuristics live, isolated for testing.
5. If `up_plan_upscale()` returns 0, `Open()` returns `VLC_EGENERIC` and
   the filter is bypassed for this stream.
6. **Calls `up_chroma_is_opaque()` (in `chroma_classify.h`) to detect
   hardware/GPU surface formats and rejects them with `VLC_EGENERIC`** —
   see "Hardware-accelerated decode" below.
7. Calls `scaler_pick()` to choose a backend (zimg if available and
   supports the chroma; swscale as fallback). Anything the picker
   doesn't recognize bypasses — better to do nothing than emit garbage.
8. Allocates `filter_sys_t`, opens the chosen backend (which spawns the
   worker pool and allocates scratch buffers), and writes the new
   dimensions into `fmt_out`.

The chroma never changes. We only resize.

## Hardware-accelerated decode

When VLC uses hardware video decode (VA-API, VDPAU, D3D9/11, MMAL,
CoreVideo on macOS) the decoder produces **opaque GPU surfaces**: a
fourcc placeholder like `VAOP` or `DX11` that points to a hardware
buffer, not a CPU pixel layout. Filters that need to read pixels (us,
postproc, deinterlace, etc.) cannot consume these directly.

`chroma_classify.h` exports `up_chroma_is_opaque()` which returns true
for the 16 known opaque chroma fourccs. `Open()` calls this very early
and returns `VLC_EGENERIC` if it matches, prompting VLC to insert a
hardware-to-software download converter upstream and re-probe us with
the resolved software chroma (typically I420).

### Lazy worker initialization

Even with the opaque-chroma reject in place, VLC's filter-chain solver
may still instantiate us multiple times during chain setup when other
filters in the chain (notably `postproc`) also reject the hardware
chroma. Each instantiation is a complete `Open()` / `Close()` round
trip. If `Open()` were to spawn the full worker pool and allocate
several MB of scratch on every call, four chain-solver probes would
cost N×4 wasted thread spawns and tens of MB of churned allocations
(N is `cores/2 − 2` from `threading.h`, e.g. 14 on a 32-core box, 30
on a 64-core box).

To avoid this, `zimg_open()` does only the cheap work — chroma
validation, geometry computation, thread-count decision, priv struct
allocation — and returns. The actual worker pool, scratch buffers, and
per-stripe filter graphs are constructed lazily on the first `Filter()`
call (`zimg_lazy_init()`). Probes that don't produce a frame cost
essentially nothing. The first frame after a successful chain
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

If any threshold is missed, AUTO falls back to 720p. RAM detection
uses Linux's `sysinfo()`. On other platforms `mem_mb` stays at 0,
which the heuristic interprets as "unknown — assume sufficient", so
non-Linux systems behave like Linux systems with plenty of RAM.

**Forward-compatibility:** unknown preset values (e.g. a future option
introduced by a different build, or a typo'd integer outside the 0–6
range) fall through to the AUTO branch. This is asserted by
`test_unknown_preset_treated_as_auto`. The plugin should never produce
nonsense output even if some downstream tool sets `target=999`.

## The actual scaling, in `Filter()`

`Filter()` is called for every decoded frame. It:

1. Allocates an output `picture_t` at the new dimensions.
2. Builds `uint8_t*[4]` plane pointers and `int[4]` strides for both
   source and destination by reading `i_planes`, `p_pixels`, and `i_pitch`
   off the `picture_t`.
3. Calls `sws_scale()` once per frame. libswscale has internal SIMD
   (SSE2/AVX2) for the common YUV planar paths.
4. If USM is enabled and the chroma is a YUV variant, applies an
   in-place unsharp mask to plane 0 (the Y plane). See "USM post-pass"
   below.
5. Copies frame metadata (timestamp, flags) via `picture_CopyProperties()`.
6. Releases the input picture.

There's no per-frame allocation beyond the output picture (which VLC
pools) and no per-frame branching on the algorithm — the scaler choice
is baked into the `SwsContext` at `Open()`, and the USM workspace is
allocated once at `Open()` and freed at `Close()`.

The swscale flags include `SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT |
SWS_FULL_CHR_H_INP` on top of the chosen algorithm. These enable proper
rounding instead of fast truncation, and full-resolution chroma
interpolation. The cost is a few percent of one core; the quality bump
is visible on saturated colour and high-contrast edges.

## USM post-pass

Lanczos is the cleanest of the three swscale algorithms but produces a
slightly soft image — a known property of any linear resampler with no
explicit edge enhancement. The classical fix is **unsharp mask**:

```
out = src + amount * (src - blur(src))
```

We use a 3×3 separable Gaussian `[1 2 1]/4 ⊗ [1 2 1]/4` for the blur,
applied only to the luma plane. The implementation lives in
`src/usm.h` as a header-only `static inline` function, just like the
upscale-decision logic, so it's exercised by both the plugin and the
test/fuzz harnesses.

Two passes over the plane:

1. *Horizontal blur* writes a `[1 2 1]/4`-filtered copy of every row
   into a contiguous workspace buffer (allocated once at `Open()`,
   sized `output_w × output_h` bytes).
2. *Vertical blur and combine* reads three rows of the workspace
   (`y-1, y, y+1` with edge replication), forms `blur(x,y)`, then
   computes `clamp(src + amount × (src − blur))` directly into the
   output picture.

`amount` is in Q8 fixed point so the inner loop has no floating-point
ops. The user-facing option `--autoupscale-usm` is a percentage; 20%
(the default) is `amount_q8 = 51`, 100% is `256`, 200% is `512`.

**Why luma only.** Sharpening the chroma planes amplifies noise in the
colour difference signal, which shows up as colour fringing on high-
contrast edges — exactly what we don't want. Sharpening packed RGB
channels independently has the same problem, so RGB chromas bypass USM
entirely. The chroma classification happens at `Open()` via a small
`ChromaHasYPlane()` helper.

**In-place safety.** `Filter()` calls `up_usm_apply_plane(dst=Y, src=Y, …)`
— same buffer for input and output. The implementation reads each row's
source pixel before writing the corresponding destination pixel within
an iteration and never re-reads it across iterations, so aliasing is
safe. There's a unit test (`apply_plane: in-place and out-of-place
produce identical output`) that checks this against a fresh-buffer
reference for a 17×13 plane with random fill.

**Performance.** Two memory-bound passes over the Y plane. On a modern
x86 core with the AVX-512 multi-versioned variant, the kernel runs
about 0.6 ns/pixel scalar-equivalent; a 1080p Y plane (≈ 2.07 MP)
costs ~1.17 ms per frame single-threaded through `up_usm_pool_apply`
(median of 5 trials, 800 iters each). The threaded pool brings this
down to ~0.23 ms with 8 stripes — about 1.4 % of a 60 fps budget.
Numbers are for amount=51 (the default 20 % USM); identity
(`amount==0`) takes a memcpy fast path with no thread activity.

**Per-deployment tunables.** `up_usm_pool_create()` takes a
`stripe_min_rows` argument (`0` = compile-time default `8`) wired to
`--autoupscale-usm-stripe-min-rows`. The zimg backend reads
`scaler_ctx_t.zimg_stripe_min_lines` (`0` = default `16`) wired to
`--autoupscale-zimg-stripe-lines`. Lower values let more workers fit
on low-resolution frames at the cost of dispatch overhead; higher
values give better load balance on tall frames. Defaults match the
historical hardcoded constants and are right for almost everyone.

### Threaded USM (`src/usm_pool.h`, `src/usm_pool.c`)

The single-threaded path (`up_usm_apply_plane` in `usm.h`) processes
the luma plane in two passes: a horizontal blur that fills a workspace
buffer, then a combine that reads three consecutive workspace rows
plus the source row to produce each output row. The threaded pool
parallelizes both passes across N workers using horizontal stripes
and a barrier between passes.

**Partition.** N workers, height H. Worker i owns rows
`[i·H/N, (i+1)·H/N)` (last worker absorbs rounding remainder). N is
clamped to `H/8` so each stripe is at least 8 rows tall — below that,
the kernel boundary handling dominates and threading hurts rather than
helps.

**Pass 1.** Each worker hblurs its own rows into a shared workspace.
No row is written by more than one worker, so this phase is naturally
race-free even though the workspace is shared.

**Barrier.** The main thread `sem_wait`s the shared "done" semaphore N
times after dispatching pass 1 (one wait per worker). Only when all N
have signaled does the main thread dispatch pass 2. This guarantees
the workspace is fully populated before any worker reads it.

**Pass 2.** Each worker combines `workspace[y-1, y, y+1]` with `src[y]`
to produce `dst[y]` for every y in its stripe. The workspace reads
just above and below the worker's stripe boundary belong to neighbor
workers, but those rows were finalized in pass 1 (which is fully
complete) so the reads are race-free.

**Lazy init.** Like the zimg backend, the USM pool's worker spawn and
workspace allocation happen on the first `apply()` call rather than
in `up_usm_pool_create()`. This means probing-only Open/Close cycles
during VLC's chain solving cost nothing for USM.

**Identity fast path.** `up_usm_pool_apply()` with `amount_q8 == 0`
short-circuits to a memcpy (or no-op when src and dst alias) with no
thread activity and no workspace allocation. So `--autoupscale-usm=0`
truly disables USM at zero cost, even if the pool was created.

**Auto-skip on grainy sources.** Independent of `amount`, the plugin
also bypasses USM after the content probe completes (frame ~60) when
the source's mean Laplacian variance exceeds
`UP_PROBE_THRESH_SHARP_LAP_MEAN` (default `3500`, exposed as
`--autoupscale-usm-sharp-threshold`). On heavily textured / grainy
content USM amplifies the noise without adding perceived sharpness, so
the plugin sets a `usm_skip_sharp` flag in `filter_sys_t` and
`ApplyUsmIfEnabled()` returns early thereafter. Toggle with
`--autoupscale-usm-skip-sharp=0` to keep USM on regardless of source.

**Optional flat-skip (compile-time).** The pool also has an opt-in
per-stripe early-out (`-DUSM_POOL_FLAT_SKIP=1` at compile time): each
worker samples its stripe's middle row in pass 1, and if horizontal
activity is below `USM_FLAT_AVG_DELTA` (≈ 2/255 average neighbour
difference) the pass-2 combine kernel is replaced by an identity copy.
Defaults to off because the implicit byte-identity guarantee against
the single-threaded reference is dropped (per-pixel delta of up to a
few LSB on borderline-flat content). Useful for benchmarking and for
content-specific builds where letterbox / fade-to-black dominate.

**Correctness verification.** The crucial invariant is that pool
output equals single-threaded output bit-for-bit. This is enforced by
`tests/test_usm_pool.c`, which runs `up_usm_apply_plane` and
`up_usm_pool_apply` on synthetic data and compares every byte. The
test exercises N = 1, 2, 3, 4, 8 workers across odd dimensions
(213×137), tall narrow (32×2000), short wide (4096×32), and the
height-clamp case (requesting 64 workers on h=16). It also runs the
same pool across 5 different frames to verify lazy init caches
correctly. End-to-end live VLC byte-identity (decoded video MD5
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
    int  (*supports)(vlc_fourcc_t chroma, int algo);
    int  (*open)   (scaler_ctx_t *);
    int  (*process)(scaler_ctx_t *, const picture_t *src, picture_t *dst);
    void (*close)  (scaler_ctx_t *);
};
```

Two implementations exist: `scaler_swscale.c` and `scaler_zimg.c`.
Either backend is free to decline a chroma it can't handle (zimg
declines NV12/NV21 and packed RGB; swscale accepts everything).
`scaler_pick()` handles failover: in `auto` mode it asks zimg first,
falls back to swscale on a `supports()` miss.

### The runtime trade-off

| Aspect            | swscale                              | zimg                                          |
|-------------------|--------------------------------------|-----------------------------------------------|
| Spline36          | no — falls back to Lanczos           | yes (native, recommended for upscaling)       |
| Chroma filter     | shared with luma                     | separately tunable (we pick the same one)     |
| Rounding          | accurate via `SWS_ACCURATE_RND` flag | accurate by default                           |
| NV12 / NV21       | yes                                  | no — needs depack/repack stage we don't add   |
| Packed RGB        | yes                                  | no                                            |
| Threading         | single-threaded per frame            | slice-threaded across N stripes via a persistent worker pool |
| Build dependency  | always present (VLC links it)        | optional — `pkg-config zimg`                  |

zimg's two wins versus swscale:

1. **Spline36** — noticeably sharper edges than Lanczos with less
   ringing on text and high-contrast detail. The recommended filter
   for upscaling and the plugin's default.
2. **Slice threading.** zimg's image-buffer model with row masks is
   built for parallel slice processing: the caller splits the
   destination into N horizontal stripes and runs
   `zimg_filter_graph_process` on each in parallel with its own
   scratch buffer. The plugin's zimg backend implements exactly this
   with a persistent worker pool — see the "Threading" section
   below for the architecture, partition rule, and the per-stripe
   independence caveat.

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

So zimg with Spline36 (slice-threaded across N stripes) is the
realistic ceiling for this plugin's architecture.

## Content-aware quality probe

Not every source benefits from upscaling. A 480p stream that's been
encoded at 200 kbps will have visible 8×8 blocking artifacts; running
Spline36 on it produces a 1080p picture where those blocks are 2.25×
larger and 2.25× more annoying. For these "lost cause" sources, the
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

1. **Laplacian variance** on the luma plane — a high-frequency-energy
   estimate. High value means the source is sharp (lots of detail
   for the scaler to interpolate from). Low value means the source
   is soft — heavily blurred, noise-reduced, or just blank — and
   there's nothing for a non-AI scaler to "uncover."

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

`src/content_probe.h` is a header-only module with three functions:

- `up_laplacian_variance(plane, stride, w, h, *n_out)` — sub-samples
  the luma plane on a 4×4 grid and accumulates squared Laplacian
  responses. ~50 µs at 480p, less at higher resolutions because the
  grid is fixed-size.

- `up_block_edge_strength(plane, stride, w, h, *n_out)` — measures
  absolute pixel differences across 8-pixel grid boundaries. Same
  4-pixel sub-sample step.

- `up_should_bypass_for_content(*accum)` — applies the decision
  rule to accumulated metrics. Returns 1 when both `lap_mean < 400²`
  and `edge_mean > 6`, and the probe has gathered enough samples.

The probe runs for 60 frames (~2 seconds at 30fps — long enough for
a few I-frames so quality varies across GOPs). After that, the
advisory either fires or doesn't, and the probe deactivates. Total
probe cost: ~3 ms for the entire window, distributed across 60
frames so no single frame is delayed perceptibly.

### Honest scope

The probe is a heuristic, not a quality measurement. It catches the
worst case — heavily compressed soft sources where upscaling
strictly hurts — and lets everything else through. Real-world
"would the upscaled picture look better than the original on the
user's display" depends on display size, viewing distance, and
personal preference; no in-filter algorithm can answer that.

The thresholds (`UP_PROBE_THRESH_SOFT_LAP_MEAN=400`,
`UP_PROBE_THRESH_BLOCKY_EDGE_MEAN=6`) are deliberately conservative.
On clean grainy 480p content the metrics typically read lap-mean
800–2000 and edge-mean 1–4, so the advisory doesn't fire. On heavily
artifact-ridden web-rips the metrics shift toward lap-mean 200–400
and edge-mean 8–15, where the advisory IS appropriate.

The same accumulator drives a second decision:
`up_should_skip_usm_for_sharpness()` returns true when `lap_mean`
exceeds `UP_PROBE_THRESH_SHARP_LAP_MEAN` (default `3500`, above the
clean-grainy 800–2000 band so it triggers only on actively
over-detailed sources — film grain, high-noise sensors, etc.). When
that fires the plugin sets `filter_sys_t.usm_skip_sharp`, and every
subsequent `ApplyUsmIfEnabled()` returns early. Both knobs are exposed
as VLC options (`--autoupscale-usm-skip-sharp` to toggle,
`--autoupscale-usm-sharp-threshold` to retune).

### Verification

The probe is observe-only — it reads source pixels, never writes
anything to the output. This is verified by decoded-MD5 comparison:
running the same source with `--autoupscale-content-probe=1` and
`--autoupscale-content-probe=0` produces byte-identical decoded
output. The 13 unit tests in `test_content_probe.c` exercise the
metric calculations on synthetic planes (flat, checkerboard,
gradient, blocky-grid) and verify the decision rule across all 5
cases (insufficient data, clean, soft-only, blocky-only, soft+blocky).

## Performance auto-tuning

### Compiler choice and SIMD

The USM kernels (`up_usm__hblur_row`, `up_usm__combine_row` in
`src/usm.h`) are autovectorization-friendly inner loops, annotated
with `restrict` on every pointer to tell the compiler the buffers
don't alias. There are two orthogonal levers that control how well
they actually vectorize.

**Lever 1: gcc cost-model pragmas.** clang -O2 vectorizes both with
width=16 out of the box. gcc -O2 alone declines on cost-model grounds
("Loop costings not worthwhile") — a known long-standing gap in gcc's
vectorizer cost model. The fix is targeted: each hot kernel is wrapped
in `#pragma GCC optimize("O3")` push/pop blocks (guarded with
`#if defined(__GNUC__) && !defined(__clang__)` so clang -Wall stays
silent on unknown pragmas). This pins -O3 to just those two functions
while everything else compiles at -O2.

Why pragma push/pop and not `__attribute__((optimize))`? The function
attribute on gcc disables `always_inline` for the attributed function,
which would defeat `static inline` and force a real call per row.
The pragma form leaves inlining decisions intact.

**Lever 2: SIMD width via `-march`.** With pragmas applied but the
default `-march=x86-64` baseline, both gcc and clang emit 16-byte
(SSE2) SIMD — the lowest-common-denominator x86_64 instruction set,
which dates to 2003. Modern CPUs support 32-byte (AVX2, 2013) and
many support 64-byte (AVX-512, 2017). Going wider doubles or
quadruples the lanes processed per instruction:

| -march level | SIMD width | 1080p USM time | Speedup vs baseline |
|---|---|---|---|
| `x86-64` (baseline) | 16 byte (SSE2) | 1709 µs | 1.0× |
| `x86-64-v3` | 32 byte (AVX2) | 892 µs | **1.92×** |
| `x86-64-v4` | 64 byte (AVX-512) | 632 µs | **2.70×** |

Numbers are median-of-5 trials of `up_usm_apply_plane` at 1080p
single-threaded. The win is consistent across all output resolutions
(480p through 4K all see 1.8–2.0× from AVX2, 2.4–2.9× from AVX-512).

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
AutoUpscale engaged: 854x480 -> 1920x1080 (... simd=default)
```

**Portable build: `make MULTIVERSION=1`.** When you need a single
binary that runs across CPU classes (e.g. distro packaging),
`MULTIVERSION=1` compiles `usm_pool.c` three times at three baselines
(SSE2 / AVX2 / AVX-512) into three separate `.o` files with renamed
public symbols. A thin dispatcher in `src/usm_pool_dispatch.c` runs at
`.so` load time via `__attribute__((constructor))`, queries
`__builtin_cpu_supports()`, and points three function pointers
(`up_usm_pool_create/destroy/apply`) at the highest-supported variant:

```c
if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw"))
    /* point at *_avx512 entry points */
else if (__builtin_cpu_supports("avx2"))
    /* point at *_avx2 entry points */
else
    /* point at *_sse2 entry points */
```

Combine `MULTIVERSION=1` with a portable `MARCH` level (e.g.
`x86-64-v3` for AVX2 baseline or `x86-64` for universal) so the rest
of the plugin (autoupscale.c, scaler*.c) also runs on the older CPUs
the dispatcher targets. The dispatch happens **once** at `dlopen`
(before VLC's plugin scanner or `vlc-cache-gen` calls into the .so),
so per-frame overhead is just one indirect call — about 1 ns on modern
x86, negligible against the millisecond-scale work it dispatches. The
chosen variant name is exported as `up_usm_pool_variant_name` and
printed in the engagement log: `AutoUpscale engaged: ... simd=avx512`.

**Why three .o files instead of `__attribute__((target_clones))`?**
`target_clones` requires non-static linkage on the cloned function,
which would defeat the `static inline` of the hot kernels in `usm.h`.
By compiling the entire usm_pool.c TU at three -march levels, each
variant gets to inline its kernels at its own SIMD width — strictly
better codegen than per-function multi-versioning. The cost is binary
size (~30 KB more for the extra two variants) and one extra .c file
in the build.

**Safety:** the AVX-512 variant's code section contains AVX-512
instructions. The dispatcher's selection logic prevents that code
from being executed on a CPU that doesn't support AVX-512, so
holding a function pointer to it on an older CPU is safe — the
dynamic linker only does relocations, no SIMD execution.

Decoded MD5 is **byte-identical** across SSE2, AVX2, AVX-512 builds
AND across MULTIVERSION=0/1 — the kernels do bytewise saturating
arithmetic, and SIMD just runs the same arithmetic in more lanes
per instruction. The exact pixel-level output you'd get from a
scalar reference build is preserved across all SIMD widths and
both build modes.

The plugin ships with the highest-quality defaults available (zimg +
Spline36 + USM 30%) and watches its own per-frame processing time so it
can tell the user when those defaults are too expensive for their
hardware.

### What's measured

Inside `Filter()`, a `clock_gettime(CLOCK_MONOTONIC)` reading is taken
just before `scaler->process()` and just after the USM post-pass. The
delta is the time the plugin itself spent on this frame — independent
of decode, downstream filters, encode, or display. The figure is fed
into `up_perfmon_record_ns()` from `src/perfmon.h`.

### EWMA, not raw samples

Per-frame timings on a real machine are noisy: a single frame can spike
from 5 ms to 50 ms because of a context switch or a cache miss. Reacting
to a single sample would produce false alarms on every busy machine.

Instead, perfmon keeps an exponentially-weighted moving average:

```
ewma <- ewma + (sample - ewma) >> 3
```

The right-shift-by-3 means α = 1/8, giving a half-life of about 5.5
samples. Sustained overruns rapidly converge the EWMA toward the true
mean; brief spikes barely move it. The "brief spike doesn't trigger"
behavior is one of the unit tests.

### Warmup and minimum-samples

Two thresholds suppress noisy early measurements:

- **Warmup (10 frames)**: dropped entirely. Cold caches, codec startup,
  and `sws_scale` / zimg internal-table construction all show up here.
- **Minimum samples (30 total)**: the EWMA is computed but not checked
  against the budget until at least 30 samples have arrived. This
  prevents the alert firing during the first second of playback while
  the EWMA is still settling.

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
5. Force 720p (target=1). 2.25× less pixel work than 1080p.

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
- **Does not measure across runs.** Each new stream gets a fresh
  perfmon. Watching twenty 480p clips in a row produces at most twenty
  hints, one per clip.

## Threading

The zimg backend slice-threads each frame: the destination is split into
N horizontal stripes processed in parallel by a persistent worker pool.
Default N = `cores/2 − 2`, clamped to `[1, 64]`. The "/ 2" reserves half
the machine for VLC's main thread, decoder, encoder, audio, vout, and
other libraries VLC pulls in; the "− 2" is an extra absolute reserve.
On a 32-core box this gives 14 workers; on 16 cores, 6; on 8 cores, 2.
Users on small machines or who measured differently can override with
`--autoupscale-threads=N`. The decision logic lives in `src/threading.h`
(header-only, exercised by `tests/test_threading.c`).

### Architecture

At Open() the backend:

1. Decides N from the user pref + detected core count.
2. Allocates pinned, page-aligned scratch buffers — one source view, one
   destination view — sized for the current stream (`src_w × src_h` and
   `dst_w × dst_h`, with subsampling and a small row of padding past the
   visible image).
3. Builds N independent `zimg_filter_graph` instances, each configured
   for a sub-image of the destination (`src_h/N → dst_h/N` for that
   stripe, dimensions aligned to 2 to satisfy chroma subsampling).
4. Spawns N persistent worker threads, each blocked on its own `sem_t`.

At each Filter() call the backend:

1. `memcpy`s VLC's source picture into the scratch source buffers.
2. Sets each worker's per-frame state, then `sem_post`s their go semaphores.
3. `sem_wait`s the shared "done" semaphore N times.
4. `memcpy`s the scratch destination into VLC's output picture
   (skipped when `--autoupscale-zerocopy-dst=1` — see below).

At Close() the backend signals exit on every worker, joins, and frees.

### Why copy-in / copy-out

Because the alternative — passing VLC's pool-managed picture buffers
directly to per-stripe zimg graphs from worker threads — is unreliable.
We isolated the failure with a layered diagnostic:

- A **standalone reproducer** (`tools/zimg_stripe_repro_threaded.c` in
  the v1 lineage) does exactly the per-stripe + threading pattern with
  fresh `aligned_alloc`'d buffers. It works at N=2, 4, 8 with Spline36,
  verified clean under valgrind and ASan.
- An **in-VLC self-test** (running the same logic from the plugin's
  Open() at startup) also works perfectly inside VLC's process address
  space.
- The same pattern using **VLC's `picture_t->p[i].p_pixels` pointers**
  segfaults inside `zimg_filter_graph_process` on the very first
  worker call, even with a mutex serializing the calls (so it's not a
  threading-safety issue between the workers — it's something specific
  to those buffers).

We didn't fully isolate the root cause inside VLC's picture allocator
(the segfault was downstream of any logging we could add, and valgrind
couldn't reach it inside VLC's process before timing out on its own
overhead). The pragmatic fix is to never hand VLC's *source* picture
buffers to zimg's per-stripe graphs — copy through scratch and pay a
few hundred MB/s of memory bandwidth for guaranteed correctness.

### Partial zero-copy on the destination side

The crash investigation specifically implicated VLC's source-pool
buffers — the buffers that come from a recycling pool managed by the
upstream filter. The destination picture is different: it comes from
`filter_NewPicture()`, which uses VLC's per-output allocator path and
is not pool-recycled (each output gets a fresh allocation). Reasoning
that the failure mode might not transfer to the dst side, the
`--autoupscale-zerocopy-dst=1` opt-in lets users skip the copy-out
entirely:

- Workers receive VLC's destination picture pointers (with VLC's pitch)
  per-frame instead of using the priv's scratch dst pointers.
- `alloc_scratch_buffers()` skips the dst allocation when zero-copy is
  on, so the scratch footprint drops by ~3 MB at 1080p.
- The per-stripe filter graphs don't bake in destination stride — it
  lives in the per-call `zimg_image_buffer` constructed by `worker_main`
  — so the strides can change per frame without rebuilding graphs.

In sandbox testing the byte-identical decoded MD5 matches between
zerocopy-dst=0 and zerocopy-dst=1 on both single-threaded and
multi-threaded runs, which is strong evidence that the zero-copy is
correct at the pixel level. With that confidence the option **defaults
to 1 (on)**: workers write directly into VLC's destination picture
and the final memcpy is skipped, saving ~125 µs per 1080p frame
(~0.8 % of a 60 fps budget). The failure mode if zero-copy
misbehaves on some VLC build is garbled output or a crash, not a soft
fallback, so the option's longtext (and the troubleshooting recipes in
[USAGE.md](USAGE.md)) tell users to set `--autoupscale-zerocopy-dst=0`
to fall back to the copy-out path if playback looks wrong.

The savings are modest: about 125 µs per 1080p frame (~0.8% of a 60 fps
budget) on this hardware. Worth shipping because some users have
specifically asked for it, but not worth recommending as a default.

### Why this isn't a regression vs single-threaded zimg

The single-threaded zimg path used to pass VLC's picture buffers to one
`zimg_filter_graph_process` call covering the full image. That worked
fine — the buffer issue only manifests with per-stripe graphs. To keep
the code paths uniform, the new threaded backend uses the scratch path
even at N=1 (the worker pool collapses to a single worker doing the
whole frame). The single-extra-memcpy at N=1 costs about 4 ms per
1080p frame, which the perfmon hint comfortably absorbs without firing
at the default 60 fps target.

### Stripe-boundary caveat

Each stripe's graph resamples independently with zimg's default boundary
handling. For natural video content the result is visually identical to
the full-frame graph; on stylized content with pixel-sharp horizontal
lines, the boundary kernel may differ slightly between adjacent stripes.
Users who care can drop to `--autoupscale-threads=1`.

## Why decision logic is in a header

`src/upscale_logic.h` is intentionally header-only and depends on neither
VLC nor FFmpeg. The plugin includes it; the tests include it; the fuzzer
includes it. The same code runs in all three places, which means:

- Tests catch logic bugs without needing VLC installed.
- The fuzzer can hammer the decision functions at ~140k execs/sec.
- Distro packagers can run `make test` in their build sandbox without
  pulling in `vlc-devel` for tests.

The trade-off is that all helper functions are `static inline`. That's fine
for this size of code — `upscale_logic.h` is under 200 lines.

## Testing strategy

The project has three layers of correctness checking, each catching a
different class of bug:

### Unit tests — exact-value assertions

178 tests across 11 suites in `tests/test_*.c`. Each test states a
specific, predictable expected value. They run under AddressSanitizer
+ UndefinedBehaviorSanitizer (`make test`), so memory errors and
signed-overflow bugs are caught even when the asserted output happens
to be correct.

| Suite                  | Count | Covers                                      |
|------------------------|-------|---------------------------------------------|
| `upscale_logic`        | 35    | preset dispatch (incl. all of 720p..8K), aspect math, ratio cap, AUTO ceiling, skip-above gating |
| `usm`                  | 17    | unsharp-mask single-thread reference        |
| `perfmon`              | 10    | EWMA performance monitor                    |
| `threading`            | 11    | thread-count decision                       |
| `zimg_helpers`         | 23    | stripe bounds, copy-plane                   |
| `chroma_classify`      | 14    | opaque chroma list + cross-predicate invariant |
| `usm_pool`             | 15    | threaded USM byte-identity vs single-thread |
| `content_probe`        | 13    | source-content metrics (variance, gradient) |
| `scaler_pick`          | 15    | backend selection logic                     |
| `lifetime`             | 10    | resource lifetime / use-after-free          |
| `usm_pool_variants`    | 15    | cross-SIMD-variant byte-equivalence (SSE2/AVX2/AVX-512) |

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

`make fuzz-smoke` runs 10 standalone harnesses (`tests/fuzz_*.c` with
`-DFUZZ_MAIN`) totalling 805k iterations under ASan + UBSan. Each
fuzzer drives a single function (or a small combination of helpers)
with deterministic xorshift32 random input and verifies a set of
post-conditions (the "invariant checker").

| Fuzzer            | Iters/run | Targets                                     |
|-------------------|-----------|---------------------------------------------|
| `upscale_logic`   | 100k      | preset dispatch, aspect math, ratio cap     |
| `usm`             | 100k      | unsharp-mask convolution                    |
| `perfmon`         |  50k      | EWMA tracker                                |
| `threading`       | 100k      | thread-count decision                       |
| `copy_plane`      |  50k      | stride-aware plane copy (memory-safety)     |
| `stripe_bounds`   | 100k      | stripe partition math                       |
| `frame_shape`     | 100k      | chroma classification + plane geometry + stripe partition together |
| `scaler_chroma`   | 100k      | chroma fourcc → zimg backend mapping (the VLC-touching boundary) |
| `content_probe`   | 100k      | source-content metric reads (pointer arithmetic on bytes) — ASan |
| `usm_variants`    |   5k      | cross-SIMD-variant byte-equivalence (3 pools per iter under ASan) |

`fuzz_upscale_logic` and `fuzz_frame_shape` use **biased input
selection** in their smoke mains. Without bias, raw int32 values
almost never hit valid presets (0–6), realistic source heights, or
real chroma fourccs — so the structured branches would go essentially
untested by random bytes. The biased mode picks valid values every
3rd–5th iteration to guarantee meaningful coverage; raw random bytes
fill the remaining iterations to keep the chaos.

### libFuzzer — coverage-guided exploration

`make fuzz` builds clang-libfuzzer targets that explore the input
space using coverage-guided mutation. CI runs five targets for 60
seconds each on every push (5 minutes total libFuzzer time):
`fuzz_upscale_logic`, `fuzz_usm`, `fuzz_frame_shape`,
`fuzz_scaler_chroma`, and `fuzz_content_probe`. The harnesses share
their invariant checker with the smoke fuzzers, so any bug found by
libFuzzer is also a violation of an explicit, named property — not
just a crash.

**Corpora.** Four structured corpora ship with the repo:

- `tests/corpus/` — 16 seeds for `fuzz_upscale_logic`, 32 bytes each
  in the format `<iiiiII>` (src_w, src_h, skip_above, preset, cores,
  mem_mb). Covers every preset value (0..6), ratio-cap boundaries,
  pathological aspects, and the historical 3×1024 regression.

- `tests/corpus_frame_shape/` — 22 seeds for `fuzz_frame_shape`, 24
  bytes each in the format `<IIiiii>` (cls_byte, garbage_fourcc, w,
  h, n_stripes, dst_h). Covers all four chroma classes (opaque,
  has-y-plane, packed, garbage), geometry boundaries, pathological
  aspects, and 8 invalid-input cases.

- `tests/corpus_scaler_chroma/` — 21 seeds for `fuzz_scaler_chroma`,
  8 bytes each in the format `<4s B 3x>` (4-byte fourcc + 1-byte
  null-pointer mask + 3 bytes pad). Covers the 4 supported chromas
  (I420/YV12/I422/I444), 9 known-rejected chromas (NV12, NV21, YUY2,
  UYVY, RV32, BGRA, VAOP, VDV0, DX11), 4 NULL-pointer combinations,
  and 3 garbage/edge fourccs (zero, all-FF, lowercase).

- `tests/corpus_usm_variants/` — 15 seeds for `fuzz_usm_variants`,
  8 bytes each in the format `<HHhBB>` (width, height, amount,
  workers, seed). Targets the cross-SIMD-variant byte-equivalence
  invariant: SIMD width boundaries (widths around 64 / 128), small
  frames, amount extremes (negative, zero, max), and worker-count
  edge cases. CI runs the smoke build (5k iters); manual libFuzzer
  runs use this corpus as the seed.

Empirically, the seeded corpora accelerate discovery roughly 2×: in
30-second runs, libFuzzer adds ~120 new corpus entries from seeds vs
~55 cold. The seeds give libFuzzer a foothold in the structured
input space — its mutations crossing-over real chroma fourccs and
real resolution heights produce more interesting inputs than blind
exploration of 32-bit space.

### Concurrency stress — TSan-instrumented worker dispatch

`make stress` exercises the `usm_pool` worker dispatch under both
ASan + UBSan and ThreadSanitizer (TSan). The threading is the most
likely place for the project to hide a bug that compiles cleanly,
passes unit tests, and *occasionally* produces wrong output — exactly
the kind of bug TSan catches even when the pixels happen to match on
this run.

Tests/stress_usm_pool.c drives 19 configurations spanning the
interesting axes:

- **Thread count saturation**: 64 workers on 32-line frames (which
  the pool clamps internally to height/8). Triggers the clamp logic.
- **Common video paths**: 854×480 at 1, 2, 4, 8, 16, 32, 64 threads,
  plus 1920×1080 at 16 threads (the realistic end-user config).
- **All USM amounts**: 0 (identity fast path, no thread spawn), 30
  (default), 100, and 200 (max).
- **Pathological aspects**: 8×1080 (tall narrow), 4096×8 (short wide
  — also triggers the height/8 clamp), odd dimensions like 853×479
  to flush odd-row handling.
- **Single-threaded large frame**: 1 worker on 4096×2160 — exercises
  the workspace allocator at scale and confirms the no-thread-spawn
  path still produces identical output.

Every frame's output is byte-compared against the single-threaded
reference `up_usm_apply_plane`. **Bit-perfect match is required** —
a single byte of divergence fails the run. The input data mutates
every frame (xorshift32-seeded fill) so workers must fetch fresh
pointers each call; a bug where workers cached stale source pointers
would surface as "first frame matches, subsequent frames diverge."

ThreadSanitizer instrumentation tracks every memory access and
synchronization event. If there were a race on the workspace buffer
(phase-1 writes vs phase-2 reads), or on the per-frame pointers, or
on the worker's `result` field, TSan would flag it even when the
output happens to be correct on this run. **Total: ~935 frames
across 19 configs (`frame_mult=1`), zero TSan reports, zero divergence.**

The CI job runs both builds end-to-end on every push. ASan+UBSan
takes ~12 seconds, TSan ~52 seconds, so the full stress step adds
about 70 seconds to CI — worth it for the confidence in the
concurrency model.

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
finding and minimizes the input. During development of
`fuzz_frame_shape`, this loop caught two real bugs in the test
harness itself — proving that the invariants are tight enough to
distinguish "wrong test" from "correct code".

### Bench tooling

`tests/bench_usm_pool.c` is a standalone perf bench separate from the
correctness tests. It accepts `<threads> <width> <height> [frames]
[amount] [fill]` and prints one CSV line of µs/frame, with
warmup-and-discard handling so worker-spawn and first-touch page
faults don't pollute the timed loop. Three fill modes (`rand` /
`flat` / `mixed`) drive different content patterns through the
kernels — useful when measuring the effect of compile-time toggles
like `USM_POOL_FLAT_SKIP`.

`scripts/bench_matrix.sh` sweeps a `(threads × resolution)` grid
(threads ∈ {1, 4, 8, 12, 16, 20}; resolution ∈ {720p, 1080p, 1440p})
and emits a CSV row per cell with the median of 3 runs. Used to A/B
the pool against itself across configurations during development.

### Coverage

`make coverage` builds a separate `cov/` test binary set with
`--coverage -fprofile-arcs -ftest-coverage`, runs them, then prints
per-file gcov summaries via `scripts/coverage_report.sh`. The script
fails the build if any tracked file falls below 80 % line coverage.
Total tracked coverage at the time of writing is 93.9 %, with the
lowest tracked file (`usm_pool.c`) at 88.8 %. `usm_pool.h` shows
`-%` because it's a pure-API header (zero executable lines), not a
coverage gap — see the project README for the explanation.

### Cyclomatic complexity

`lizard` is the standard tool. `make analyze` runs cppcheck only;
complexity is enforced manually as a project rule: every function in
`src/` must have CCN ≤ 10, and every function in `tests/` must have
CCN ≤ 9. Verified by `lizard -C 11 src` (warns at 11+) and
`lizard -C 10 tests` (warns at 10+). Helpers extracted for
complexity stay `static` (or `static inline` for header-only modules)
and live in the same file as their caller.
