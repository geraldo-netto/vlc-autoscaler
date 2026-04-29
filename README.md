# VLC AutoUpscale

A VLC video filter that detects sub-720p video and upscales it in real time
to 720p or 1080p, picking the target automatically from available CPU and
RAM (or by explicit override).

The actual scaling is done with **libswscale** (FFmpeg's resampler, which
VLC already links against). It supports fast bilinear, bicubic, and Lanczos.
Lanczos is the default — it's the cleanest of the three while still being
fast enough for real-time playback on modest hardware.

> **Note.** This is a classical-DSP upscaler, not an AI one. It cannot
> "invent" detail that wasn't in the source the way Real-ESRGAN or Anime4K
> can. What it does is fit the picture to your panel sharply and without
> the soft, slightly-smeared look you get from VLC's default output-stage
> scaler. See [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md) for why.

## Status

| Check                                  | Result                                  |
|----------------------------------------|-----------------------------------------|
| Unit tests (ASan + UBSan)              | 141/141 pass across 9 suites            |
| Smoke fuzz (670k iters, ASan + UBSan)  | pass across 9 fuzzers                   |
| libFuzzer (60s × 5 in CI, seeded)      | 0 crashes; corpora accelerate discovery ~2× |
| Concurrency stress (ASan + TSan)       | 19 configs, ~1300 frames, 0 races       |
| `cppcheck` (warning + style)           | clean                                   |
| Plugin compiles against VLC 3.0.20     | clean, no warnings                      |
| Live transcode (zimg + swscale)        | verified end-to-end up to 8K            |
| GitHub Actions CI                      | runs on push/PR to `main` and `develop` |

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
> [`docs/USAGE.md`](docs/USAGE.md) has 8 recipes covering: minimal use,
> bypassing the recursion limit, recommended-default (no postproc),
> hardware-decode-safe playback, quality-first, low-latency, and
> file output — plus a step-by-step **diagnostic ladder** for tracking
> down black screens, freezes, or garbage output, and a section on
> getting audio to start at 100% volume on Linux.

## Does it run automatically when I play a video?

Not by default — VLC video filters are opt-in. You have three ways to
turn it on:

1. **Per-launch:** `vlc --video-filter=autoupscale myvideo.mp4`
2. **GUI:** Tools → Preferences → "Show settings: All" → Video → Filters →
   tick **AutoUpscale**.
3. **Config file:** add `video-filter=autoupscale` to `~/.config/vlc/vlcrc`.

Once enabled, it runs on every video, but the filter's `Open()` callback
returns "not interested" for sources already ≥ 720p, so VLC bypasses the
filter entirely with **zero per-frame cost** for HD content. Practical
setup: enable once, leave it on, only sub-HD content is touched.

## Options

| Option                       | Range  | Default | Meaning                                                  |
|------------------------------|--------|---------|----------------------------------------------------------|
| `--autoupscale-target`       | 0–6    | 0       | 0 = auto (720p/1080p based on HW), 1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 4K, 5 = 5K, 6 = 8K. AUTO never picks above 1080p — higher targets must be explicit. All targets are subject to a 4× linear ratio cap relative to source. |
| `--autoupscale-algo`         | 0–3    | **3**   | 0 = bilinear, 1 = bicubic, 2 = lanczos, **3 = spline36** |
| `--autoupscale-skip-above`   | 1+     | 720     | Source heights ≥ this value are passed through untouched |
| `--autoupscale-usm`          | 0–200  | 30      | Unsharp-mask amount (%) applied to luma post-upscale     |
| `--autoupscale-backend`      | 0–2    | 0       | 0 = auto (zimg → swscale), 1 = zimg only, 2 = swscale only |
| `--autoupscale-target-fps`   | 0–240  | 60      | Per-frame work over `1 / target_fps` triggers a one-time tuning hint. 0 disables monitoring. |
| `--autoupscale-threads`      | 0–64   | 0       | Slice the frame into N horizontal stripes processed in parallel. 0 = auto (`cores/2 − 2`), 1 = single-threaded, 2..64 = explicit. |
| `--autoupscale-zerocopy-dst` | 0–1    | **1**   | 1 = workers write directly into VLC's destination picture (default). Saves ~125 µs/frame at 1080p. Set to 0 to use the copy-out path if you see garbled output or crashes. |
| `--autoupscale-content-probe`| 0–1    | **1**   | 1 = run the diagnostic content probe on the first ~60 frames to detect heavily-compressed soft sources where upscaling actively hurts. Logs a one-time advisory when triggered. Observe-only — never modifies output. 0 = skip the probe. |

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
slightly tighter rounding by default. **swscale** is the universal
fallback, used automatically when zimg isn't compiled in or doesn't
support the input chroma (NV12/NV21, packed RGB).

| Mode      | Behavior                                              |
|-----------|-------------------------------------------------------|
| `auto` (0) | Try zimg, fall back to swscale on chroma mismatch    |
| `zimg` (1) | zimg only; filter declines if zimg can't handle it   |
| `swscale` (2) | swscale only; always engages, maps Spline36 → Lanczos |

If you build the plugin on a system without `libzimg-dev`, the zimg
backend simply isn't compiled in and `auto` always uses swscale. The
build prints `zimg backend: ENABLED` or `disabled` so you know which.

### Performance auto-tuning

The plugin defaults to the **highest-quality** settings (zimg + Spline36
+ luma USM at 30%) and watches its own per-frame processing time. If the
exponentially-weighted average of frame work exceeds your target
(`--autoupscale-target-fps`, default 60), the plugin emits a one-time
hint with concrete tuning suggestions:

```
autoupscale filter: AutoUpscale engaged: 854x480 -> 1920x1080 (backend=zimg algo=3 ...)
autoupscale filter: Performance warning: avg frame work is 22341 us, exceeding the
                    60 FPS budget of 16666 us (backend=zimg algo=3 usm=30).
autoupscale filter:   To tune down, try (in order of decreasing quality cost):
autoupscale filter:     --autoupscale-algo=2   (lanczos: similar quality, faster)
autoupscale filter:     --autoupscale-algo=1   (bicubic: noticeably faster, slight quality drop)
autoupscale filter:     --autoupscale-usm=0    (disable post-sharpening)
autoupscale filter:     --autoupscale-backend=2 (force swscale: faster scaler)
autoupscale filter:     --autoupscale-target=1 (force 720p instead of 1080p: 2.25x less work)
autoupscale filter:   Or set --autoupscale-target-fps=0 to silence this warning.
```

The hint fires once per stream after a 10-frame warmup window plus 30
sample-frame minimum, so brief codec-startup spikes don't trigger false
alarms. Set `--autoupscale-target-fps=0` to disable monitoring entirely.

### Threading

The plugin slice-threads the zimg backend by splitting each output frame
into N horizontal stripes processed in parallel by a persistent worker
pool. Default = `cores/2 − 2`, capped at `[1, 64]`; on a 32-core box that's
14 workers, on a 16-core box 6, on an 8-core box 2. The "/ 2" reserves
half the machine for the rest of VLC (decoder, encoder, audio, vout) plus
other libraries; the "− 2" is an extra absolute reserve. Override with
`--autoupscale-threads=N` (1 keeps the single-threaded fast path; or set
explicitly to a higher value if you measured otherwise on your hardware).

**Implementation note:** the plugin maintains pinned, page-aligned
scratch buffers and copy-in / copy-out per frame: VLC's source picture
is `memcpy`'d into the scratch source, threaded zimg runs on the scratch
buffers, the scratch destination is `memcpy`'d to VLC's output picture.
A 480p I420 frame is ~615 KB in and a 1080p frame is ~3.1 MB out, so the
copies cost a few hundred MB/s of memory bandwidth — invisible against
modern memory's 50+ GB/s and dwarfed by the parallelism win.

The scratch path on the **source** side is mandatory: passing VLC's
pool-managed picture buffers directly to per-stripe zimg graphs from
worker threads is unreliable (we couldn't fully isolate the cause
through instrumentation, but the pure pattern works in standalone tests
and in-VLC self-tests on fresh buffers). The src memcpy trades a small
bandwidth cost for correctness.

The scratch path on the **destination** side is opt-out via
`--autoupscale-zerocopy-dst=1`. VLC's destination picture comes from
`filter_NewPicture()`, which uses a different allocator than the
source-pool buffers that crashed under the per-stripe pattern. With
zero-copy enabled, workers write directly into the VLC dst picture and
the final memcpy is skipped, saving ~125 µs/frame at 1080p
(~0.8% of a 60 fps budget). This is opt-in because we can't fully
verify it across every VLC configuration, and a misconfiguration here
shows up as garbled output or a crash rather than a soft failure. The
default is the safe copy-out path; turn zero-copy on if you've measured
that the savings matter on your hardware and verified that playback is
stable.

The swscale backend stays single-threaded (it's the universal-fallback
backend; threading is a quality-tier-only nicety here).

**Stripe boundary caveat:** each stripe's graph resamples independently
with zimg's default boundary handling. For natural video content the
effect is invisible; on stylized content with pixel-sharp horizontal
lines a user can fall back to `--autoupscale-threads=1`.

The USM (unsharp mask) post-pass compensates for any resampler's slight
softening, applied as a 3×3 separable Gaussian high-pass on the luma
plane only. Defaults to 30% (subtle); 100% gives a clearly sharper
look, 200% is aggressive. Only applied to YUV chromas — sharpening
packed RGB or chroma planes causes visible colour fringing on edges.
Set to `0` to disable.

**Threaded USM.** USM uses the same worker count as the scaler
(`--autoupscale-threads`). The luma plane is partitioned into N
horizontal stripes; each worker hblurs its rows into a shared
workspace (pass 1), then a synchronization barrier ensures the
workspace is fully populated, and finally each worker combines
`workspace[y-1, y, y+1]` with `src[y]` to produce `dst[y]` on its rows
(pass 2). The pool is lazy: workers and workspace spawn on the first
frame, not in `Open()`. When `--autoupscale-usm=0` the pool's identity
fast path skips all thread activity, so disabling USM truly costs
nothing. At 1080p luma this drops USM from ~2-3 ms/frame
single-threaded to ~0.5 ms with 8 stripes — roughly 12% of a 60 fps
budget. Output is bit-identical to the single-threaded path; that's
verified end-to-end by the unit tests (`test_usm_pool.c` runs both
implementations on synthetic data and asserts byte-for-byte match).

### Hardware-accelerated decode

If your VLC is using hardware video decode (VA-API, VDPAU, D3D9/11,
MMAL, CoreVideo) the decoder produces **opaque GPU surfaces** that the
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
and returns. The 30-worker-thread pool and 6 MB of scratch are
allocated on the first `Filter()` call. This way, when VLC's chain
solver instantiates us 3-4 times during chain probing (which happens
when combined with other filters that reject hardware chromas), the
probes that don't produce a frame cost essentially nothing.

### The "Too high level of recursion (3)" error

If you combine `--video-filter='postproc:autoupscale'` with hardware
decode, VLC's filter chain solver may give up with:

```
chain filter error: Too high level of recursion (3)
```

This is a **hard-coded limit in VLC core** (`MAX_CHAIN_LEVEL` in
`src/misc/filter_chain.c`), not something the plugin can fix.

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

   **If frames still drop at startup**, it's the vout module probing
   (gl/glx/egl_x11 switches, font database build, hw-decode probes)
   eating ~500 ms before the first frame can display. Add caching and
   relax the late-frame heuristic:

   ```bash
   vlc --file-caching=3000 --clock-jitter=0 \
       --autoupscale-threads=16 \
       --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=postproc:autoupscale}:display' \
       video.mp4
   ```

   `--file-caching=3000` gives the pipeline 3 s of demuxer pre-roll
   (default is 1000 ms). `--clock-jitter=0` disables the heuristic
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

4. **Patch VLC's `MAX_CHAIN_LEVEL`.** The most surgical fix if you
   build VLC yourself. Bump the constant from 3 to 4 or 5 in
   `src/misc/filter_chain.c` and rebuild. One-line patch — included
   in this repo at
   [`patches/vlc-3.0-raise-max-chain-level.patch`](patches/vlc-3.0-raise-max-chain-level.patch).
   Lets `postproc + autoupscale + hwaccel` chains succeed without any
   plugin or filter-syntax changes. Note: this lets the chain
   *construct*, but the upscale-then-downscale waste still happens
   unless you also use option 1. Send the patch upstream while you're
   at it.

**What we did fix from the plugin side:** lazy worker init means each
of those 3-4 chain-solver probes used to cost 30 thread spawns and
~24 MB of scratch allocations (90+ wasted thread spawns and ~72 MB
churn per playback start). With lazy init the failed probes spawn
nothing — verified end-to-end in user logs: 4 `AutoUpscale engaged`
messages but only 1 `zimg: 30 worker threads` message, meaning only
the chain that actually plays paid for the worker pool.

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
   src_h ≥ skip-above ?  ──── yes ───► passthrough (zero overhead)
        │ no
        ▼
   pick target height (auto / 720 / 1080)
        │
        ▼
   compute even-rounded width preserving aspect
        │
        ▼
   sws_scale() with the chosen algorithm + accurate-rounding flags
        │
        ▼
   YUV chroma? ─── yes ───► luma USM post-pass
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
  perfmon.h               pure EWMA perf monitor (header-only)
  threading.h             pure thread-count decision (header-only)
  zimg_helpers.h          pure pitch/lines/plane/stripe-bounds helpers
  chroma_classify.h       pure hwaccel/Y-plane chroma predicates
  scaler_zimg_chroma.h    pure chroma->zimg-subsample mapping (extracted for fuzzing)
  content_probe.h         pure no-reference quality probe (Laplacian + block-edge)
  scaler.h                backend interface
  scaler.c                backend picker (auto / zimg / swscale)
  scaler_swscale.c        libswscale backend (universal fallback)
  scaler_zimg.c           libzimg backend with slice-threaded copy-in/out
  autoupscale.c           VLC plugin glue (module descriptor, Open/Filter/Close)

tests/
  test_*.c                unit tests for each pure header (no framework)
  fuzz_*.c                libFuzzer + deterministic smoke targets (one source each)
  stress_usm_pool.c       concurrency stress, runs under ASan and TSan
  corpus/                 binary seeds for fuzz_upscale_logic (16 files, 32 bytes each)
  corpus_frame_shape/     binary seeds for fuzz_frame_shape (22 files, 24 bytes each)
  corpus_scaler_chroma/   binary seeds for fuzz_scaler_chroma (21 files, 8 bytes each)

docs/HOW_IT_WORKS.md      design notes
docs/USAGE.md             command-line recipes + diagnostic ladder
patches/                  optional VLC patches (workaround for chain depth limit)
.github/workflows/ci.yml  build, test, smoke fuzz, stress, libFuzzer, cppcheck
Makefile                  everything (`make help` lists targets)
```

The decision/math logic lives in `upscale_logic.h` as `static inline`
functions with zero VLC and FFmpeg dependencies, so it can be tested and
fuzzed standalone. The plugin and the test suite both `#include` the same
header — there's no shadow re-implementation in the tests.

## Building from source

```sh
make            # build the VLC plugin (libautoupscale_plugin.so)
make plugin     # same
make test       # unit tests under ASan + UBSan (141 tests across 9 suites)
make fuzz-smoke # 670k deterministic random inputs across 9 fuzzers under ASan + UBSan
make fuzz       # libFuzzer build (clang); run e.g. build/fuzz_upscale_logic tests/corpus/
make stress     # usm_pool concurrency stress, ASan + TSan (~70s)
make analyze    # cppcheck across the source
make install    # install plugin into VLC's plugins dir
make uninstall
make clean
make info       # show pkg-config paths and toolchain
```

**CPU baseline:** The default Makefile passes `-march=x86-64-v3` to the
compiler, which targets the AVX2 + BMI2 + FMA instruction set (Intel
Haswell 2013+, AMD Zen 1 2017+). This is what makes the vectorized
USM kernels emit **32-byte AVX2 vectors instead of 16-byte SSE2** —
measured ~1.9× speedup on the kernel work at every output resolution
(480p through 4K) and consistent across both gcc and clang.

| Setting | SIMD width | Speedup vs SSE2 | Hardware |
|---|---|---|---|
| `MARCH=x86-64` | 16 byte (SSE2) | 1.0× (baseline) | runs anywhere x86-64 |
| `MARCH=x86-64-v3` (**default**) | 32 byte (AVX2) | **~1.9×** | Haswell 2013+ / Zen 1 2017+ |
| `MARCH=x86-64-v4` | 64 byte (AVX-512) | ~2.7× | Skylake-X 2017+ / Zen 4 2022+ |
| `MARCH=native` | whatever this CPU has | varies | this build host only |

Override examples:

```sh
make MARCH=x86-64-v4    # AVX-512 if your CPU has it
make MARCH=native       # tune for the build host
make MARCH=x86-64       # legacy fallback for pre-2013 Intel / pre-2017 AMD
make MARCH=             # no -march flag at all
```

The decoded output is **byte-identical** across all SIMD widths
(verified by reproducible MD5) — wider vectors run the same arithmetic
in more lanes, not different arithmetic.

**Compiler choice:** Both gcc and clang produce competitive SIMD with
the AVX2 baseline. `CC=clang make plugin` is still slightly faster on
some kernels but the gap is now ~1.2× rather than the ~3× it was with
SSE2. Either works.

`make test`, `make fuzz-smoke`, `make stress`, and `make analyze` do **not** need VLC
headers — only `make plugin` does. This is intentional so distro packagers
and CI can run the test suite without a VLC dev install.

## Requirements

- **For the plugin:** VLC ≥ 3.0 with development headers, FFmpeg
  (`libswscale`, `libavutil`) with development headers, `gcc` or `clang`,
  `make`, `pkg-config`.
- **For tests / fuzz / static analysis only:** `gcc` or `clang`. The
  libFuzzer target additionally needs `clang` with `-fsanitize=fuzzer`
  (any reasonably recent clang is fine).

## VLC 3.x vs 4.x

This plugin targets VLC 3.x, which is what every distro currently ships.
VLC 4.x has changed `pf_video_filter` and a few config helpers; a port is
straightforward but not done here.

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

**Picture jitters or audio drifts** — your CPU can't keep up with Lanczos
at the chosen target. Drop to `--autoupscale-algo=1` (bicubic) or force
720p with `--autoupscale-target=1`.
