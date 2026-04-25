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
| Unit tests (ASan + UBSan)              | 42/42 pass (19 logic + 13 USM + 10 perfmon) |
| Smoke fuzz (150k iters, ASan + UBSan)  | pass                                    |
| libFuzzer (5 min, seeded corpus)       | 9.6M+ execs across both targets, 0 crashes |
| `cppcheck` (warning + style)           | clean                                   |
| Plugin compiles against real VLC 3.0.20| clean, no warnings                      |
| Live transcode through zimg + swscale  | both verified, 854×480 → 1920×1080      |
| GitHub Actions CI                      | runs all of the above on every push     |

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
```

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
| `--autoupscale-target`       | 0–2    | 0       | 0 = auto, 1 = force 720p, 2 = force 1080p                |
| `--autoupscale-algo`         | 0–3    | **3**   | 0 = bilinear, 1 = bicubic, 2 = lanczos, **3 = spline36** |
| `--autoupscale-skip-above`   | 1+     | 720     | Source heights ≥ this value are passed through untouched |
| `--autoupscale-usm`          | 0–200  | 30      | Unsharp-mask amount (%) applied to luma post-upscale     |
| `--autoupscale-backend`      | 0–2    | 0       | 0 = auto (zimg → swscale), 1 = zimg only, 2 = swscale only |
| `--autoupscale-target-fps`   | 0–240  | 60      | Per-frame work over `1 / target_fps` triggers a one-time tuning hint. 0 disables monitoring. |
| `--autoupscale-threads`      | 0–64   | 0       | Slice the frame into N horizontal stripes processed in parallel. 0 = auto (`cores − 2`), 1 = single-threaded, 2..64 = explicit. |

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
pool. Default = `cores − 2`, capped at `[1, 64]`; on a 24-core box that's
22 workers. Override with `--autoupscale-threads=N` (1 keeps the
single-threaded fast path).

**Implementation note:** the plugin maintains pinned, page-aligned
scratch buffers and copy-in / copy-out per frame: VLC's source picture
is `memcpy`'d into the scratch source, threaded zimg runs on the scratch
buffers, the scratch destination is `memcpy`'d to VLC's output picture.
A 480p I420 frame is ~615 KB in and a 1080p frame is ~3.1 MB out, so the
copies cost a few hundred MB/s of memory bandwidth — invisible against
modern memory's 50+ GB/s and dwarfed by the parallelism win.

The scratch path is mandatory because passing VLC's pool-managed picture
buffers directly to per-stripe zimg graphs from worker threads is
unreliable (we couldn't fully isolate the cause through instrumentation,
but the pure pattern works in standalone tests and in-VLC self-tests on
fresh buffers). Going through scratch trades a tiny memcpy cost for
correctness.

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
src/upscale_logic.h         pure logic (header-only, no VLC/FFmpeg deps)
src/autoupscale.c           VLC plugin: module descriptor, Open/Filter/Close
tests/test_upscale_logic.c  unit tests (no framework, just CHECK macros)
tests/fuzz_upscale_logic.c  libFuzzer target + smoke runner (one source)
tests/corpus/               seed inputs for libFuzzer
docs/HOW_IT_WORKS.md        design notes
.github/workflows/ci.yml    build, test, smoke fuzz, libFuzzer, cppcheck
Makefile                    everything (`make help` lists targets)
```

The decision/math logic lives in `upscale_logic.h` as `static inline`
functions with zero VLC and FFmpeg dependencies, so it can be tested and
fuzzed standalone. The plugin and the test suite both `#include` the same
header — there's no shadow re-implementation in the tests.

## Building from source

```sh
make            # build the VLC plugin (libautoupscale_plugin.so)
make plugin     # same
make test       # unit tests under ASan + UBSan
make fuzz-smoke # 100k deterministic random inputs under ASan + UBSan
make fuzz       # libFuzzer build (clang); run with build/fuzz_upscale_logic corpus/
make analyze    # cppcheck across the source
make install    # install plugin into VLC's plugins dir
make uninstall
make clean
make info       # show pkg-config paths and toolchain
```

`make test`, `make fuzz-smoke`, and `make analyze` do **not** need VLC
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

## License

GPL-2.0-or-later, matching VLC core. See [`LICENSE`](LICENSE).
