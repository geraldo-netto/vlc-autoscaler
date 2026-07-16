# Usage and troubleshooting

Start with the simplest command. Add overrides only to solve a measured problem.

## Playback

Direct VLC filter chain:

```sh
vlc --video-filter=autoupscale path/to/video.mp4
```

This is the lowest-overhead path. VLC may compensate for the filter's format
change before display, however, so the renderer may not receive the enlarged
frame dimensions.

Transcode display path:

```sh
vlc --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=autoupscale}:display' path/to/video.mp4
```

Use this when direct playback reports `Too high level of recursion (3)` or when
the enlarged dimensions must reach the display. It adds a real-time encode and
decode, so it costs more CPU. Keep audio in the same transcode pipeline to avoid
parallel-path drift.

For damaged legacy video, test deblocking before scaling:

```sh
vlc --postproc-q=6 --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=postproc:autoupscale}:display' path/to/video.mp4
```

Do not enable `postproc` by default. Missing decoder quantization data can make
it ineffective or unstable, and clean sources do not benefit.

## Common profiles

Force 1080p:

```sh
vlc --video-filter=autoupscale --autoupscale-target=2 path/to/video.mp4
```

Reduce work when playback misses its frame budget:

```sh
vlc --video-filter=autoupscale --autoupscale-target=1 --autoupscale-algo=1 --autoupscale-usm=0 path/to/video.mp4
```

Use conservative zimg buffer paths:

```sh
vlc --video-filter=autoupscale --autoupscale-backend=1 --autoupscale-zerocopy-src=0 --autoupscale-zerocopy-dst=0 path/to/video.mp4
```

Disable hardware decode when VLC cannot construct the converter chain:

```sh
vlc --avcodec-hw=none --video-filter=autoupscale path/to/video.mp4
```

Write an upscaled file:

```sh
vlc -I dummy --no-audio --autoupscale-target=2 --sout='#transcode{vcodec=h264,vb=12000,venc=x264{preset=medium,crf=18},vfilter=autoupscale}:standard{access=file,mux=mp4,dst=output.mp4}' --play-and-exit path/to/input.mp4
```

Verify its dimensions:

```sh
ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate output.mp4
```

## Tuning order

When the performance advisory appears, change one setting at a time:

1. `--autoupscale-algo=2` for Lanczos.
2. `--autoupscale-algo=1` for bicubic.
3. `--autoupscale-usm=0` to remove sharpening work.
4. `--autoupscale-backend=2` to force swscale.
5. `--autoupscale-target=1` to force 720p.

Leave `--autoupscale-threads=0` unless same-host measurements show a better
value. More workers can increase dispatch, cache, and memory-bandwidth costs.
Pinning is best-effort and on by default; disable it only when measurements on
the deployment host show a regression.

Do not change `--autoupscale-zimg-stripe-lines` as a general performance knob.
It can change the zimg row/column grid; source-direct and copy-in paths can
then have bounded partition seams rather than byte-identical output. Keep the
validated automatic value (`0`, selecting 16) unless both output and throughput
tests support another value on the deployment host.

## Diagnose output failures

Run these checks in order with the same input:

1. Confirm VLC works without the plugin:

   ```sh
   vlc path/to/video.mp4
   ```

2. Enable only AutoUpscale:

   ```sh
   vlc --video-filter=autoupscale path/to/video.mp4
   ```

3. Force zimg and disable direct picture access:

   ```sh
   vlc --video-filter=autoupscale --autoupscale-backend=1 --autoupscale-zerocopy-src=0 --autoupscale-zerocopy-dst=0 path/to/video.mp4
   ```

4. Remove concurrency and USM:

   ```sh
   vlc --video-filter=autoupscale --autoupscale-backend=1 --autoupscale-threads=1 --autoupscale-target=1 --autoupscale-usm=0 --autoupscale-zerocopy-src=0 --autoupscale-zerocopy-dst=0 path/to/video.mp4
   ```

Interpretation:

- Failure without the plugin points to VLC, decoding, output, or the input.
- Success with copy paths points to source or destination zero-copy handling.
- Success with one worker points to grid/thread behavior or throughput.
- Success without USM points to the sharpening path.
- Forced zimg declining means the build or chroma does not support zimg; repeat
  the baseline with backend `2` to test swscale.

Capture a private diagnostic log:

```sh
log=$(mktemp "${TMPDIR:-/tmp}/autoupscale.XXXXXX")
chmod 600 "$log"
vlc --verbose=2 --video-filter=autoupscale path/to/video.mp4 2>&1 | tee "$log"
grep -iE 'autoupscale|filter|chroma|recursion|error|fail|warning' "$log"
```

Include the exact command, plugin commit, VLC version, source dimensions/chroma,
engagement line, and relevant errors in a bug report. Remove or redact media
paths, URLs, and credentials before sharing the log.

## Known VLC interactions

### Hardware decode

The plugin cannot read opaque GPU surfaces. It declines them so VLC can insert
a CPU-readable conversion and retry. If chain construction fails, use the
transcode display path or `--avcodec-hw=none`. The optional
`patches/vlc-3.0-raise-chain-level.patch` raises VLC 3.0's converter-chain limit
for users who build VLC themselves; it does not prevent the direct display
chain from resizing output again.

### Audio drift and late frames

First determine whether processing is slower than the source frame budget. If
so, use the tuning order above. If the issue persists without AutoUpscale, fix
the VLC/audio path instead. Caching can absorb startup or input jitter but
cannot fix sustained throughput:

```sh
vlc --file-caching=3000 --network-caching=3000 path/to/video.mp4
```

Choose the correct audio device in VLC or with your PipeWire/PulseAudio tools.
Do not use compressor gain as a substitute for an unmuted output stream.

### Plugin not engaged

Check registration and verbose logs:

```sh
vlc --list | grep autoupscale
vlc --verbose=2 --video-filter=autoupscale path/to/video.mp4
```

AUTO intentionally declines sources at or above `skip-above` (720p by default),
plans that would downscale, and incompatible chromas. Use an explicit target
only when you intend to process a source above the AUTO threshold.

## Related guides

- [Configuration and build reference](../README.md)
- [Architecture](ARCHITECTURE.md)
- [Performance and benchmarking](BENCHMARKS.md)
- [Cinnamon/Nemo integration](DESKTOP_INTEGRATION.md)
