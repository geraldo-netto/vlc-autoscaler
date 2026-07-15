# VLC AutoUpscale

A VLC video filter that detects sub-720p video and upscales it in real time
to 720p or 1080p, picking the target automatically from available CPU and
RAM (or by explicit override).

Supported deployment target: **Linux on x86-64 with VLC 3.x**. Other operating
systems, architectures, and VLC 4 are outside this project's compatibility
contract.

The scaling is done by either **libzimg** (preferred when present) or
**libswscale** (FFmpeg's resampler, which VLC already links against).
zimg adds Spline36 to the bilinear / bicubic / Lanczos lineup. Spline36
is the default — sharper than Lanczos for upscaling with less ringing,
and still fast enough for real-time playback on modest hardware.

> **Note.** This is a classical-DSP upscaler, not an AI one. It cannot
> "invent" detail that wasn't in the source the way Real-ESRGAN or Anime4K
> can. What it does is fit the picture to your panel sharply and without
> the soft, slightly-smeared look you get from VLC's default output-stage
> scaler. See [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md) for why.

## Verification

| Check | Source of truth |
|-------|-----------------|
| Unit and sanitizer tests | `make test` |
| Deterministic smoke fuzzing | `make fuzz-smoke` |
| Coverage-guided fuzzing | `make fuzz` and the CI workflow |
| Concurrency stress | `make stress` and `make stress-zimg` |
| Static analysis | `make analyze`; `make scan-build` checks both production plugin configurations |
| Cyclomatic complexity | `make complexity` enforces CCN ≤ 10 |
| Per-file and per-function coverage | `make coverage` enforces the configured gate |
| Plugin build | `make plugin EXTRA_CFLAGS=-Werror` |
| zimg integration | `make test-zimg` |

These commands calculate their own scope and thresholds. Their live output is
authoritative; this document intentionally does not duplicate totals or
percentages that can drift as the project changes.

## Quick start

```sh
# 1. Install build dependencies
#    Debian/Ubuntu:
sudo apt install build-essential pkg-config \
                 libvlccore-dev libvlc-dev \
                 libswscale-dev libavutil-dev \
                 libzimg-dev   # OPTIONAL — enables the higher-quality zimg backend
#    Fedora:
sudo dnf install gcc make pkgconfig vlc-devel ffmpeg-devel zimg-devel

# 2. Build the plugin
make
# The build prints "zimg backend: ENABLED" or "disabled" so you know which.

# 3. Install it into VLC's plugin tree
sudo make install
# If VLC doesn't pick it up, regenerate the cache (path printed by `make info`):
sudo vlc-cache-gen /usr/lib/x86_64-linux-gnu/vlc/plugins

# 4. Try it on a sub-720p file
vlc --video-filter=autoupscale path/to/lowres.mp4

# NOTE: with --video-filter, VLC downscales the upscaled output back to
# the source's display size. To actually feed 1080p frames to your screen
# (and to interoperate cleanly with hardware decode), use the transcode
# pipeline instead. Use vcodec=h264 — VLC 3.0.20's mp4v encoder fails on
# modern FFmpeg (6.x) due to legacy option parsing bugs. Use
# preset=ultrafast — the intermediate H.264 stream is decoded immediately
# for display, so encoding quality doesn't matter, only speed does.
# Include acodec=mp4a so audio runs through the same pipeline as video
# (without it, audio takes a parallel path that desyncs):
vlc --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=autoupscale}:display' \
    path/to/lowres.mp4

# See "Hardware-accelerated decode" section below for why this matters.
```

> **Want command-line recipes for specific situations?**
> [`docs/USAGE.md`](docs/USAGE.md) has recipes covering minimal use,
> bypassing the recursion limit, recommended-default (no postproc),
> hardware-decode-safe playback, quality-first, low-latency, and
> file output — plus a step-by-step **diagnostic ladder** for tracking
> down black screens, freezes, or garbage output, and a section on
> getting audio to start at 100% volume on Linux.

> **Cinnamon/Nemo desktop integration:**
> [`docs/CINNAMON-DESKTOP-ACTIONS.md`](docs/CINNAMON-DESKTOP-ACTIONS.md)
> installs a separate “VLC (AutoUpscale)” launcher, Open With entry, and
> right-click action without replacing the stock VLC launcher.

## Does it run automatically when I play a video?

Not by default — VLC video filters are opt-in. You have three ways to
turn it on:

1. **Per-launch:** `vlc --video-filter=autoupscale myvideo.mp4`
2. **GUI:** Tools → Preferences → "Show settings: All" → Video → Filters →
   tick **AutoUpscale**.
3. **Config file:** add `video-filter=autoupscale` to `~/.config/vlc/vlcrc`.

Once enabled, it runs on every video. In AUTO, the filter's `Open()` callback
returns "not interested" for sources already ≥ 720p, so VLC bypasses the
filter entirely with **zero per-frame cost** for HD content. Explicit target
presets intentionally bypass that gate, though the no-downscale and 4×-ratio
rules still apply.

## Options

| Option                              | Range  | Default | Meaning                                                  |
|-------------------------------------|--------|---------|----------------------------------------------------------|
| `--autoupscale-target`              | 0–6    | 0       | 0 = auto (720p/1080p based on HW), 1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 4K, 5 = 5K, 6 = 8K. AUTO never picks above 1080p — higher targets must be explicit. All targets are subject to a 4× linear ratio cap relative to source. |
| `--autoupscale-algo`                | 0–3    | **3**   | 0 = bilinear, 1 = bicubic, 2 = lanczos, **3 = spline36** |
| `--autoupscale-skip-above`          | 0–8192 | 720     | In AUTO, source heights ≥ this value are passed through untouched; 0 disables the gate |
| `--autoupscale-usm`                 | 0–200  | **20**  | Unsharp-mask amount (%) applied to luma post-upscale     |
| `--autoupscale-backend`             | 0–2    | 0       | 0 = auto (zimg → swscale), 1 = zimg only, 2 = swscale only |
| `--autoupscale-target-fps`          | 0–240  | 60      | Per-frame work over `1 / target_fps` triggers a one-time tuning hint. 0 suppresses the tuning hint; EWMA/stat telemetry remains active. |
| `--autoupscale-threads`             | 0–64   | 0       | Worker preference shared by zimg and USM. zimg normally uses row stripes and may add column cells on wide/short frames; each pool can clamp lower for frame geometry. 0 = auto (`cores/2 − 2`); 1..64 = explicit preference, capped by CPUs allowed through taskset/cgroup affinity. The USM pool additionally caps itself at 12 workers — the pass is memory-bandwidth-bound and measurably regresses past that (set `--autoupscale-usm-stripe-min-rows` to take over the partition policy and bypass the cap). A pool that resolves to a single worker runs on the calling thread with no thread or barrier at all. swscale remains single-threaded. |
| `--autoupscale-pin-threads`         | 0–1    | 0       | 1 = pin each zimg worker round-robin across the exact CPU IDs allowed by the process affinity mask (Linux, best-effort). Off by default — pinning can hurt on a typical desktop by fighting the scheduler; enable only on a dedicated high-core/NUMA transcode box where you measured a gain. Does not affect the USM pool. |
| `--autoupscale-zerocopy-dst`        | 0–1    | **1**   | 1 = aligned row-only graphs write directly into VLC's destination picture (default); column cells still copy tile scratch out. Avoids a frame-sized copy on the row-only path. Set to 0 to force copy-out. |
| `--autoupscale-zerocopy-src`        | 0–1    | **1**   | 1 = worker graphs read VLC's source picture directly (default; no copy-in). Set to 0 to copy each source stripe into scratch first if a particular VLC build misbehaves on the source-pool buffers. |
| `--autoupscale-content-probe`       | 0–1    | **1**   | 1 = log a diagnostic advisory after the fixed initial observation window when a source is both soft and blocky. Observe-only — never modifies output. 0 = suppress the advisory; metrics still run when nonzero USM and `--autoupscale-usm-sharp-threshold` need them. |
| `--autoupscale-usm-stripe-min-rows` | 0–256  | 0       | Minimum rows per USM worker stripe (0 = compile-time default 8). Smaller values let more workers fit on low-res frames at the cost of dispatch overhead. |
| `--autoupscale-zimg-stripe-lines`   | 0–128  | 0       | Minimum dst lines per zimg worker stripe (0 = compile-time default 16). Same trade-off as above for the scaler backend. |
| `--autoupscale-usm-sharp-threshold` | 0–20000 | **3500** | Mean squared Laplacian-response cutoff above which the source is considered heavily textured/grainy and the USM post-pass is skipped for the rest of playback (USM amplifies grain without adding sharpness on such content). `0` disables the feature (USM always runs); higher values trip rarely; lower values trip aggressively. |

The plugin defaults to **Spline36 on zimg** — the highest-quality
combination available. On systems without zimg, swscale transparently
maps Spline36 → Lanczos (and logs the fallback at debug level), so the
defaults give you the best each backend can offer.

In auto mode, the plugin picks 1080p only if all three are true:
≥ 4 CPU cores, ≥ 2 GB RAM (or RAM unknown), and the upscale ratio
to 1080p stays within 4× the source height. Otherwise it picks 720p.

### Backend selection

The plugin has two interchangeable scaler backends. **zimg** is preferred
when available — it has Spline36 (sharper than Lanczos for upscaling
with less ringing), separately tuned luma and chroma filters, and
slightly tighter rounding by default. **swscale** is the broad-coverage
fallback for supported CPU-readable chromas, used automatically
when zimg isn't compiled in, misses support, fails to open, or reports a
fatal processing failure.

| Mode      | Behavior                                              |
|-----------|-------------------------------------------------------|
| `auto` (0) | Prefer zimg; use swscale on support miss or zimg open failure, and switch once after a fatal zimg processing failure |
| `zimg` (1) | Strict zimg only; never falls back                   |
| `swscale` (2) | swscale only for supported CPU-readable chromas; maps Spline36 → Lanczos |

Transient picture or geometry failures drop only that frame and do not change
the active backend. If you build the plugin without `libzimg-dev`, the zimg
backend simply isn't compiled in and `auto` always uses swscale. The
build prints `zimg backend: ENABLED` or `disabled` so you know which.

### Performance auto-tuning

The plugin defaults to the **highest-quality** settings (zimg + Spline36
+ luma USM at 20%) and watches its own per-frame processing time. If the
exponentially-weighted average of frame work exceeds your target
(`--autoupscale-target-fps`, default 60), the plugin emits a one-time
hint with concrete tuning suggestions:

```
autoupscale filter: AutoUpscale engaged: <source> -> <target> (backend=... algo=...)
autoupscale filter: Performance warning: avg frame work is <measured> us,
                    exceeding the configured frame budget.
autoupscale filter:   To tune down, try (in order of decreasing quality cost):
autoupscale filter:     --autoupscale-algo=2   (lanczos: similar quality, faster)
autoupscale filter:     --autoupscale-algo=1   (bicubic: noticeably faster, slight quality drop)
autoupscale filter:     --autoupscale-usm=0    (disable post-sharpening)
autoupscale filter:     --autoupscale-backend=2 (force swscale: faster scaler)
autoupscale filter:     --autoupscale-target=1 (force 720p instead of 1080p)
autoupscale filter:   Or set --autoupscale-target-fps=0 to silence this warning.
```

The hint fires once per stream after the fixed warmup and sample windows
defined in `src/perfmon.h`, so brief codec-startup spikes don't trigger false
alarms. Set `--autoupscale-target-fps=0` to silence this advisory; the EWMA,
periodic stats, and exported variables remain active.

### Exported VLC variables

For external monitoring, the filter publishes live counters as integer object
variables on its own `filter_t`. They are updated on the same ~5 s tick as the
periodic `frames=… dropped=… ewma=…` debug line, so they carry no per-frame
cost. Read them with `var_GetInteger()` from an embedder that holds the filter
object, or observe them via VLC's variable callbacks.

| Variable                | Meaning                                             |
| ----------------------- | --------------------------------------------------- |
| `autoupscale-ewma-us`   | Current EWMA of per-frame processing time, in µs.   |
| `autoupscale-frames`    | Frames processed since the filter opened.           |
| `autoupscale-dropped`   | Frames dropped (pool exhaustion or backend failure).|

The variables exist only while the filter instance is open. If VLC cannot
create them at open time the filter logs a one-shot warning and skips the
export; playback is unaffected.

### Threading

The plugin grid-threads zimg, normally as horizontal stripes and with optional
column cells on wide/short frames. The default preference is `cores/2 − 2`,
capped by the supported range and the process affinity mask. Geometry may
reduce the effective count. The formula reserves capacity for VLC's decoder,
encoder, audio, vout, and other libraries. Override with
`--autoupscale-threads=N`; the explicit preference is still capped by the
process affinity mask and each pool's geometry minimum.

The shared completion wait has a 10-second monotonic deadline so a lost wake
cannot park VLC's video-output thread forever. This is a synchronization-fault
detector, not a hard execution deadline: failure retires the pool and joins
its workers synchronously, and an already-running zimg or USM callback must
finish before its storage can be released.

**Implementation note:** both options default to zero-copy. On an aligned,
row-only grid, graphs read VLC's source and write its destination directly.
First-frame misalignment switches the affected side to persistent,
64-byte-aligned plane scratch. Column grids always copy private destination
tile scratch into the output sub-rectangle. Either option can force its copy
path; source copy-in also changes a column grid to rows-only.

Pointing per-stripe graphs at VLC's pool-managed buffers from worker
threads was historically unreliable, so each side stays independently
opt-out. The destination path proved the pattern works; the source path is
its symmetric twin, and the shared picture-view validator checks chroma,
negotiated crop origin, plane count, pointers, pitches, and row extents before
dispatch. Crops that violate zimg's 32-byte direct-buffer alignment switch the
first frame to aligned scratch I/O; unsafe later storage drift drops that frame.
If a particular VLC build misbehaves — garbled
output or a crash — set either side back to the copy path with
`--autoupscale-zerocopy-src=0` and/or `--autoupscale-zerocopy-dst=0`.
Copy/zero-copy modes are byte-identical when they keep the same grid. On a
wide/short frame, forcing source copy-in also changes a column grid to
rows-only, so only the documented bounded graph-seam deltas may differ.

The swscale backend stays single-threaded (it's the broad-coverage fallback
backend; threading is a quality-tier-only nicety here).

**Stripe boundary caveat:** each horizontal stripe's graph resamples its
own source rows with zimg's default vertical boundary handling, so a
stripe boundary carries a sub-pixel phase rounding (≤ a few code values on
real content — invisible; only synthetic high-frequency patterns make it
measurable). On stylized content with pixel-sharp horizontal lines a user
can fall back to `--autoupscale-threads=1`. Column tiles read cross-boundary
source context via zimg's `active_region`, but independent column graphs can
also restart resize phase and introduce bounded seam deltas.

The USM (unsharp mask) post-pass compensates for any resampler's slight
softening, applied as a 3×3 separable Gaussian high-pass on the luma
plane only. Defaults to 20% (subtle); 100% gives a clearly sharper
look, 200% is aggressive. Only applied to YUV chromas — sharpening
packed RGB or chroma planes causes visible colour fringing on edges.
Set to `0` to disable.

The plugin also detects heavily textured/grainy sources after the configured
observation window (via the content probe's squared-Laplacian
energy metric) and
auto-skips the USM post-pass for those, since USM on grainy content
amplifies noise without adding perceived sharpness. Tunable through
`--autoupscale-usm-sharp-threshold` — set the cutoff lower to trip
on more sources, higher to trip rarely, or `0` to disable the feature
entirely.

**Threaded USM.** USM shares `--autoupscale-threads` as its preference but
clamps independently to its stripe minimum; swscale itself remains
single-threaded. The luma plane is partitioned into N horizontal stripes;
each worker sweeps its rows once with three rolling blur rows plus two
in-place halo snapshots, fusing the horizontal blur and the
`combine(blur[y-1,y,y+1], src[y]) → dst[y]` step (PERF-2). There is no
shared workspace and no mid-frame barrier — workers read neighbor source
rows read-only and write only their own dst rows, so the sweep is
race-free. The pool is lazy: workers and private scratch initialize on the
first USM apply, not in `Open()`. With `--autoupscale-usm=0`, the plugin does
not create or call the pool, so disabling USM has no pool activity. Each
`usm_worker_t` payload is `alignas(64)`, and its `aligned_alloc`'d array
places every payload on its own cache-line boundary. The payload holds only
stripe geometry, scratch pointers, and per-frame pixel state. Aligned thread
records and generation tracking live in the shared `worker_pool.h` lifecycle;
its broadcast and done gate is implemented in `threading.h`. Both USM and zimg
use that shared machinery. Output is bit-identical to the single-threaded
path; that's verified end-to-end by the unit tests (`test_usm_pool.c`
runs both implementations on synthetic data and asserts byte-for-byte
match).

### Hardware-accelerated decode

If your VLC is using hardware video decode (VA-API or VDPAU on the supported
Linux target), the decoder produces **opaque GPU surfaces** that the
autoupscale filter cannot read directly. autoupscale detects this and
fails Open() with a debug-level message:

```
autoupscale filter debug: AutoUpscale: declining opaque chroma 0x504f4156;
                          VLC will insert a hw->sw converter and re-probe
```

VLC then inserts a hardware-to-software download converter and re-probes
us with the resolved software chroma (typically I420). Filtering
proceeds normally from there, but you pay the GPU→CPU readback cost on
every frame.

**Lazy worker init.** Open() is intentionally cheap: it validates
chroma compatibility, computes geometry, allocates a small priv struct,
and returns. The worker pool and its scratch storage are
allocated on the first valid `Filter()` picture. This way, when VLC's chain
solver instantiates the filter speculatively during chain probing (which happens
when combined with other filters that reject hardware chromas), the
probes that don't produce a frame cost essentially nothing.

### The "Too high level of recursion (3)" error

If you combine `--video-filter='postproc:autoupscale'` with hardware
decode, VLC's filter chain solver may give up with:

```
chain filter error: Too high level of recursion (3)
```

This is a **hard-coded limit in VLC** (`CHAIN_LEVEL_MAX` in
`modules/video_chroma/chain.c`, default `2` — error fires at level
> 2 = 3), not something the plugin can fix.

**Where the recursion actually happens** (verified from a full debug
log): it's *not* in your `postproc → autoupscale` chain itself. That
chain constructs fine. The recursion fires in VLC's "compensate for
format changes" sub-chain, which VLC inserts after autoupscale to
convert our 1440×1080 I420 output back to whatever the display expects
(640×480 VDV0 with VDPAU, etc.). That sub-chain needs:

1. swscale: resize 1440×1080 I420 → 640×480 I420
2. chroma converter: 640×480 I420 → 640×480 (some intermediate)
3. chroma converter: 640×480 (intermediate) → 640×480 VDV0/VAOP

Three converters in series, each one costing one level of recursion.
With software decode the chroma chain is shorter (display takes I420
directly, no VDV0 conversion needed), so the budget fits. Add hwaccel
and the budget runs out.

**There's a deeper issue this exposes:** even when the chain succeeds,
the compensation step downscales autoupscale's 1440×1080 output back
to your display's source-buffer size (typically the original
resolution, like 640×480). So in standard `--video-filter=` usage you
pay for the upscale work and OpenGL re-scales the downscaled buffer to
fit your screen — meaning **you don't visibly benefit from autoupscale's
quality.** The workaround below addresses both problems.

**Workarounds, ranked by what they fix:**

1. **`--sout '#transcode{vfilter=postproc:autoupscale}:display'`** —
   The genuine fix. Transcode operates on an encoded stream and feeds
   the upscaled output to the display directly, with no "compensate
   for format changes" sub-chain. The display renders 1440×1080
   buffers at full quality, and the recursion limit doesn't apply
   because there's no compensation chain to construct.

   The naive form drops frames in real-time playback because x264's
   default `preset=medium` is too slow to keep up at 50+ fps while
   competing with autoupscale's worker threads for cores. **Use
   `preset=ultrafast` and `tune=zerolatency`** — the intermediate
   H.264 stream is decoded immediately for display, so encoding
   quality doesn't matter; only speed does. Also **include
   `acodec=mp4a`** so audio runs through the same sout pipeline as
   video — without it, audio takes a parallel path that desyncs:

   ```bash
   vlc --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=postproc:autoupscale}:display' video.mp4
   ```

   On a many-core system, also consider leaving cores for the encoder
   by capping autoupscale's thread count. With 32 cores, splitting
   16/16 between autoupscale and x264 avoids starvation and runs
   smoother than 30/all:

   ```bash
   vlc --autoupscale-threads=16 \
       --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=postproc:autoupscale}:display' \
       video.mp4
   ```

   **Use `vcodec=h264`, not `mp4v`** — VLC 3.0.20's mp4v encoder passes
   legacy options (`qsquish`, `border_mask`, `noise_reduction`, `lmin`,
   `lmax`, `rc_buffer_aggressivity`) that FFmpeg 6.x has removed,
   causing `cannot open mp4v video encoder`. h264, h265, and hevc all
   use modern option pipelines that work cleanly with FFmpeg 6.x. This
   is a VLC core bug, unrelated to autoupscale.

   **If audio still doesn't play**, your default audio sink may be a
   Bluetooth device that's off or out of range. Check with `pactl info |
   grep "Default Sink"` and switch with
   `pactl set-default-sink alsa_output.pci-XXXX.hdmi-stereo-extra2`
   (your HDMI sink name from `pactl list short sinks`), or pick the
   right device in VLC: Audio → Audio Device.

   **If frames still drop at startup**, vout module probing
   (gl/glx/egl_x11 switches, font database build, hw-decode probes)
   may delay the first displayable frame. Add caching and
   relax the late-frame heuristic:

   ```bash
   vlc --file-caching=3000 --clock-jitter=0 \
       --autoupscale-threads=16 \
       --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=postproc:autoupscale}:display' \
       video.mp4
   ```

   `--file-caching=3000` requests 3 s of demuxer pre-roll (the default varies
   by VLC build/input). `--clock-jitter=0` disables the heuristic
   that aggressively drops late frames before the cache has time to
   smooth things out. These help with the *startup hump*; they don't
   fix sustained throughput deficits — for those you need lower bitrate,
   a smaller upscale target, or fewer chained filters.

2. **`--avcodec-hw=none`** — disable hardware decode for the session.
   The hw→sw converter goes away; the compensation chain stays short
   enough to fit the recursion budget. The upscale-then-downscale
   waste *still* happens (you don't visibly benefit from autoupscale's
   quality unless you also use option 1), but at least playback won't
   thrash on chain rebuilds.

3. **Drop one filter.** With hardware decode, `postproc + autoupscale`
   together exceed the budget. Either filter alone fits:

   ```bash
   vlc --video-filter='autoupscale' video.mp4   # autoupscale only with hwaccel
   vlc --video-filter='postproc' video.mp4      # postproc only with hwaccel
   ```

   Same upscale-vs-display caveat applies if you use autoupscale via
   `--video-filter`.

4. **Patch VLC's `CHAIN_LEVEL_MAX`.** The most surgical fix if you
   build VLC yourself. Bump the constant from 2 to 5 in
   `modules/video_chroma/chain.c` and rebuild. One-line patch —
   included in this repo at
   [`patches/vlc-3.0-raise-chain-level.patch`](patches/vlc-3.0-raise-chain-level.patch),
   verified against VLC 3.0.20.
   Lets `postproc + autoupscale + hwaccel` chains succeed without any
   plugin or filter-syntax changes. Note: this lets the chain
   *construct*, but the upscale-then-downscale waste still happens
   unless you also use option 1. Send the patch upstream while you're
   at it.

**What we did fix from the plugin side:** lazy worker init prevents speculative
chain-solver probes from spawning workers or allocating frame scratch. Only
the chain that receives a valid picture pays for the worker pool.

## Better quality: chain with VLC's postproc filter

For low-bitrate sources (DVD rips, old web video, anything with visible
blocking or ringing), VLC's built-in `postproc` filter — which uses
libpostproc for deblocking and deringing — combines very well with
this one. Apply postproc *first* so the cleaner picture goes into the
upscaler:

```sh
vlc --video-filter='postproc:autoupscale' --postproc-q=6 lowres.mp4
```

The order matters. `postproc:autoupscale` deblocks, *then* upscales,
*then* sharpens. Reversing the order would amplify compression artifacts
before cleaning them up.

## How it decides what to do

```
   source frame
        │
        ▼
   AUTO and src_h ≥ skip-above ?  ─ yes ─► passthrough (zero overhead)
        │ no
        ▼
   pick target height (auto / 720 / 1080 / 1440 / 4K / 5K / 8K)
        │
        ▼
   compute even-rounded width preserving aspect
        │
        ▼
   active backend process() (zimg workers or sws_scale)
        │
        ▼
   eligible YUV + USM enabled? ─ yes ─► optional luma post-pass
        │ no
        ▼
   upscaled frame to renderer
```

Full design notes in [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md).

## Repository layout

```
src/
  upscale_logic.h         pure resolution-decision math (no VLC/FFmpeg deps)
  usm.h                   pure unsharp-mask post-pass (header-only)
  usm_pool.h              public API for the threaded USM worker pool
  usm_pool.c              threaded USM implementation (lazy worker spawn)
  usm_pool_dispatch.c     runtime SSE2/AVX2/AVX-512 dispatcher (MULTIVERSION=1)
  usm_pool_variants.h     X-macro shared SIMD-variant API declarations
  cpu_level.h             runtime x86-64 v3/v4 capability probes
  perfmon.h               pure EWMA perf monitor (header-only)
  threading.h             CPU topology/thread policy + shared dispatch gate
  worker_pool.h           shared lazy worker lifecycle, dispatch, and teardown
  plane_utils.h           backend-neutral plane copy + stripe/tile partitioning
  zimg_helpers.h          zimg scratch and chroma-plane geometry helpers
  chroma_classify.h       shared software-chroma metadata + classification
  scaler_zimg_chroma.h    shared descriptor-to-zimg adapter
  content_probe.h         pure no-reference quality probe (Laplacian + block-edge)
  picture_view.h          validated crop-aware physical-plane views
  scaler_pick_logic.h     pure backend selection + open-fallback logic
  scaler_status.h         transient-frame vs fatal-backend status contract
  scaler.h                backend interface
  scaler.c                backend picker (auto / zimg / swscale)
  scaler_swscale.c        libswscale backend (broad-coverage fallback)
  scaler_zimg.c           lazy libzimg row/column worker grid with selectable I/O
  autoupscale.c           VLC plugin glue (module descriptor, Open/Filter/Close)

tests/
  test_*.c                unit tests for each pure header (no framework)
  fuzz_*.c                libFuzzer + deterministic smoke targets (one source each)
  stress_usm_pool.c       concurrency stress, runs under ASan and TSan
  bench_usm_pool.c        standalone perf bench (no verification)
  corpus/                 binary seeds for fuzz_upscale_logic
  corpus_frame_shape/     binary seeds for fuzz_frame_shape
  corpus_scaler_chroma/   8/17-byte mapping and crop seeds for fuzz_scaler_chroma
  corpus_usm_variants/    binary seeds for fuzz_usm_variants

scripts/
  bench_matrix.sh         run bench_usm_pool across (threads × resolution × fill)
  coverage_per_function.sh
                          enforce the per-function gcov threshold
  coverage_report.sh      gcov per-file summary used by `make coverage`
  install-vlc-autoupscale-action.sh
                          install/uninstall the Cinnamon/Nemo integration
  vlc-autoupscale.sh      VLC AutoUpscale launcher wrapper installed above

docs/HOW_IT_WORKS.md      design notes
docs/USAGE.md             command-line recipes + diagnostic ladder
docs/BENCHMARKS.md        reproducible benchmark procedure and interpretation
docs/PERFORMANCE.md       compatibility pointer to the benchmark guide
docs/CINNAMON-DESKTOP-ACTIONS.md
                          Cinnamon/Nemo launcher and right-click integration
patches/                  optional VLC patches (workaround for chain depth limit)
.github/workflows/ci.yml  build, test, fuzz, coverage, and analyzer CI
sonar-project.properties  SonarQube analysis scope and exclusions
Makefile                  build, test, fuzz, stress, coverage, and analysis targets
```

The decision/math logic lives in `upscale_logic.h` as `static inline`
functions with zero VLC and FFmpeg dependencies, so it can be tested and
fuzzed standalone. The plugin and the test suite both `#include` the same
header — there's no shadow re-implementation in the tests.

## Building from source

```sh
make             # build the VLC plugin (libautoupscale_plugin.so)
make plugin      # same
make test        # unit/contract suite under ASan + UBSan
make fuzz-smoke  # deterministic fuzz suite under ASan + UBSan
make fuzz        # libFuzzer build (clang); run e.g. build/fuzz_upscale_logic tests/corpus/
make stress      # usm_pool concurrency stress, ASan + TSan
make coverage    # gcov per-file and per-function gates
make analyze     # cppcheck across the source
make scan-build  # Clang Static Analyzer, single- and multiversion plugins
make install     # install plugin into VLC's plugins dir
make uninstall
make clean
make info        # show pkg-config paths and toolchain
```

**CPU baseline (defaults).** This plugin is a source distribution —
every user builds it on the same machine they run it on — so the
defaults target the **build host** rather than the oldest x86-64 ISA
baseline supported by Linux:

| Variable | Default | Why |
|---|---|---|
| `MARCH` | `native` | Tunes for the local CPU and may enable additional ISA features. |
| `MULTIVERSION` | `0` | Avoids shipping runtime variants that a build-host-tuned binary does not need. |

The chosen SIMD level is logged in VLC at engagement time. Default
build on a Zen 4 box prints `simd=default` (single-baseline, the
compiler emitted whatever `-march=native` allows). A multi-versioned
build prints `simd=avx512` / `avx2` / `sse2` to identify which runtime
variant was picked:

```
AutoUpscale engaged: <source> -> <target> (… simd=default)
```

**Build presets.** Pick whichever fits your distribution model:

```sh
# Default — tuned for the build host; another CPU must support the emitted ISA.
make

# ISA-compatible AVX-512 build (Skylake-X 2017+ / Zen 4 2022+).
make MARCH=x86-64-v4

# AVX2-baseline build (Haswell 2013+ / Zen 1 2017+) with runtime
# SIMD variant selection.
make MARCH=x86-64-v3 MULTIVERSION=1

# Linux x86-64/SSE2 baseline build with runtime USM variants.
make MARCH=x86-64 MULTIVERSION=1
```

When `MULTIVERSION=1` the plugin links three copies of `usm_pool.c`
compiled at SSE2 / AVX2 / AVX-512 baselines, plus a thin runtime
dispatcher (`usm_pool_dispatch.c`) that selects one at `.so` load time via
`up_cpu_supports_v3()` / `up_cpu_supports_v4()`. Supported compilers probe the
complete levels directly. The compatibility fallback for older compilers does
not enumerate every inherited feature, so deploy those builds only to CPUs
known to satisfy the selected level. Per-frame overhead after selection is an
indirect call.

The decoded output is **byte-identical** across all SIMD widths and
across `MULTIVERSION=0` / `=1` (verified by reproducible MD5 and the
cross-variant byte-equivalence test) — wider vectors run the same
arithmetic in more lanes, not different arithmetic.

**Compiler choice:** Both gcc and clang are supported for the SIMD variants.
Measure on the deployment host if compiler choice matters for throughput.

`make test`, `make fuzz-smoke`, `make stress`, and `make analyze` do not need
VLC headers. The zimg harness targets and production analysis use the external
SDKs listed below; swscale and picture-view contract tests use local stubs.

## Requirements

- **Platform:** Linux x86-64. Other OS/architecture combinations are not
  supported or portability-tested.
- **For the plugin:** VLC 3.x and FFmpeg (`libswscale`, `libavutil`)
  development headers/libraries, a C compiler, `make`, and `pkg-config`;
  libzimg development files are optional.
- **For tests / smoke fuzz / stress:** the compiler selected by `CC`, with
  pthreads and ASan, UBSan, and TSan support. `make test` also uses Python 3.
- **For libFuzzer:** Clang with `-fsanitize=fuzzer`; select it with `CLANG`.
- **For coverage:** a matched `COV_CC` / `GCOV` pair, Bash, Python 3, an `awk`
  implementation, and GNU coreutils (including `realpath`).
- **For zimg verification:** VLC and zimg development files. The optional
  `test-zimg`, `stress-zimg`, `bench-zimg`, and `coverage-zimg` targets use the
  compiler selected by `CC`.
- **For static analysis:** `cppcheck` and `lizard`. `make scan-build` also
  needs Clang's `scan-build` (`clang-tools` on Debian/Ubuntu), plus the VLC and
  FFmpeg development files used by the production plugin; zimg remains
  optional.
- **For guarded build cleanup:** Git and GNU `realpath`.

## VLC 3.x vs 4.x

This plugin targets VLC 3.x. VLC 4.x has changed `pf_video_filter` and a few
config helpers; it is outside the current compatibility contract.

## Troubleshooting

**`vlc-plugin pkg-config not found`** — install `libvlccore-dev`
(Debian/Ubuntu) or `vlc-devel` (Fedora).

**Plugin builds but VLC doesn't see it** — rebuild the cache:
`sudo vlc-cache-gen <pluginsdir>` (path printed by `make info`).

**`Unsupported chroma 0x...` in the log** — the decoder is feeding a chroma
this filter doesn't map to swscale yet. The common ones (I420, NV12, YV12,
I422, I444, RGB24/RGBA/BGRA) are already handled. Hardware-decoded video
paths (VAAPI/VDPAU) hand off opaque GPU surfaces and will hit this branch;
disable hw decoding with `--avcodec-hw=none` to force a CPU-readable
surface.

**Picture jitters or audio drifts** — your CPU can't keep up with the selected
scaler (Spline36 by default) at the chosen target. Drop to
`--autoupscale-algo=1` (bicubic) or force
720p with `--autoupscale-target=1`.
