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
   `backend`, `target-fps`, `threads`).
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
and returns `VLC_EGENERIC` if it matches — *before* allocating any
scratch buffers or spawning worker threads. This matters because VLC's
filter-chain solver probes filters multiple times during chain setup;
without the early reject we'd allocate 30 worker threads × 3 probes =
90 wasted thread spawns before VLC tears us down.

After the reject, VLC inserts a hardware-to-software download converter
upstream and probes us once more with the resolved software chroma
(typically I420), at which point Open() succeeds normally. The user
pays a GPU→CPU readback cost per frame but everything else works.

If the user combines `--video-filter='postproc:autoupscale'` with
hardware decode, VLC's chain solver may still hit `Too high level of
recursion (3)` because both filters need software pixels and the solver
has to thread converters around them. The clean workaround is to
disable hardware decode for that session: `--avcodec-hw=none`.

The list of opaque fourccs is unit-tested (`test_chroma_classify.c`)
including a cross-predicate invariant that no opaque chroma is also
flagged as having a luma plane (which would let USM run on a GPU
surface — a guaranteed crash).

## The auto-target heuristic

When `target=0` (auto), the plugin picks 720p or 1080p based on three
inputs:

| Signal              | Threshold for 1080p       |
|---------------------|---------------------------|
| CPU cores           | ≥ 4                       |
| Total RAM (Linux)   | ≥ 2 GB, or unknown        |
| Upscale ratio       | ≤ 4× (i.e. `src_h * 4 ≥ 1080`) |

If any threshold is missed, it falls back to 720p. The ratio cap exists
because non-AI scalers (Lanczos included) don't recover detail — they only
reconstruct missing pixels by interpolation, and a 5×+ upscale produces a
softer, ringy result that's worse than just letting the player scale on
output. 4× is a generous ceiling.

RAM detection uses Linux's `sysinfo()`. On other platforms `mem_mb` stays
at 0, which the heuristic interprets as "unknown — assume sufficient", so
non-Linux systems behave like Linux systems with plenty of RAM.

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
ops. The user-facing option `--autoupscale-usm` is a percentage; 30%
(the default) is `amount_q8 = 76`, 100% is `256`, 200% is `512`.

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
x86 core with `-O2`, roughly 2 ns/pixel, so a 1080p Y plane (≈ 2.07 MP)
costs ~4 ms per frame on one core. Well inside a 30 fps frame budget;
not worth vectorizing given how cheap it already is.

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
| Threading         | single-threaded per frame            | single-threaded (caller-driven slicing is a planned follow-up) |
| Build dependency  | always present (VLC links it)        | optional — `pkg-config zimg`                  |

zimg's main near-term win is **Spline36**: noticeably sharper edges
than Lanczos with less ringing on text and high-contrast detail.
That alone is worth the abstraction. The threading story is the longer
game — zimg's image-buffer model with row masks is designed for parallel
slice processing, where the caller splits the destination into N
horizontal stripes and runs `zimg_filter_graph_process` on each in
parallel with its own scratch buffer. For 24 cores upscaling 480p →
1080p, that should give close to linear speedup. Not implemented yet.

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

So zimg with Spline36 is the realistic ceiling for this plugin's
architecture. The follow-up that's worth doing is parallel slicing
inside zimg's `process()` to actually use the cores.

## Performance auto-tuning

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
Default N = `cores − 2`, clamped to `[1, 64]`. The decision logic lives
in `src/threading.h` (header-only, exercised by `tests/test_threading.c`).

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
4. `memcpy`s the scratch destination into VLC's output picture.

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
overhead). The pragmatic fix is to never hand VLC's picture buffers to
zimg's per-stripe graphs in the first place — copy through scratch and
pay a few hundred MB/s of memory bandwidth for guaranteed correctness.

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
