# VLC AutoUpscale

VLC AutoUpscale is a Linux x86-64 video filter for VLC 3.x. It enlarges
low-resolution video in real time with zimg (preferred) or FFmpeg swscale,
then optionally sharpens luma with an unsharp-mask pass.

It is a classical resampler, not an AI upscaler. It improves presentation but
cannot recover detail absent from the source.

## Install

Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libvlccore-dev libvlc-dev libswscale-dev libavutil-dev libzimg-dev
make
sudo make install
```

Fedora:

```sh
sudo dnf install gcc make pkgconfig vlc-devel ffmpeg-devel zimg-devel
make
sudo make install
```

`libzimg-dev`/`zimg-devel` is optional. Without it, the plugin uses swscale.
Run `make info` to inspect detected paths and features. If VLC does not find the
installed plugin, rebuild the plugin cache at the path shown by `make info`:

```sh
sudo vlc-cache-gen <vlc-plugins-directory>
```

## Use

Enable the filter for one launch:

```sh
vlc --video-filter=autoupscale path/to/video.mp4
```

AUTO mode skips sources at or above 720p. For a persistent setup, enable
AutoUpscale under **Tools → Preferences → All → Video → Filters**, or add
`video-filter=autoupscale` to `~/.config/vlc/vlcrc`.

VLC's direct display chain can resize the filter output back to the decoded
size or hit `Too high level of recursion (3)`, especially with hardware decode.
Use the transcode display path when you need the upscaled frame dimensions to
reach the renderer:

```sh
vlc --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=autoupscale}:display' path/to/video.mp4
```

See [usage and troubleshooting](docs/USAGE.md) for practical variants.

## Options

| Option | Range | Default | Effect |
|---|---:|---:|---|
| `--autoupscale-target` | 0–6 | 0 | `0` AUTO; `1` 720p; `2` 1080p; `3` 1440p; `4` 4K; `5` 5K; `6` 8K. All targets have a 4× linear ratio cap. |
| `--autoupscale-algo` | 0–3 | 3 | Bilinear, bicubic, Lanczos, or Spline36. swscale maps Spline36 to Lanczos. |
| `--autoupscale-skip-above` | 0–8192 | 720 | In AUTO, skip sources at or above this height. `0` disables the gate. |
| `--autoupscale-usm` | 0–200 | 20 | Luma sharpening percentage. `0` disables USM. |
| `--autoupscale-backend` | 0–2 | 0 | `0` prefer zimg with swscale fallback; `1` zimg only; `2` swscale only. |
| `--autoupscale-target-fps` | 0–240 | 60 | Budget for the one-time performance hint. `0` hides the hint. |
| `--autoupscale-threads` | 0–64 | 0 | Shared zimg/USM worker preference. `0` uses the automatic policy. |
| `--autoupscale-pin-threads` | 0–1 | 0 | Best-effort zimg worker pinning. Enable only after measuring. |
| `--autoupscale-zerocopy-dst` | 0–1 | 1 | Direct zimg writes on compatible row grids. `0` forces copy-out. |
| `--autoupscale-zerocopy-src` | 0–1 | 1 | Direct zimg reads. `0` forces copy-in and disables column tiling. |
| `--autoupscale-content-probe` | 0–1 | 1 | Emit one advisory for soft and blocky sources. It never changes output. |
| `--autoupscale-usm-stripe-min-rows` | 0–256 | 0 | Minimum USM rows per worker. `0` selects 8. |
| `--autoupscale-zimg-stripe-lines` | 0–128 | 0 | Minimum zimg output lines per stripe. `0` selects 16. |
| `--autoupscale-usm-sharp-threshold` | 0–20000 | 3500 | Skip USM on grainy sources above this metric. `0` disables skipping. |

AUTO chooses 1080p only with at least four available CPUs, at least 2 GiB RAM
(or unknown RAM), and an upscale ratio no greater than 4×. Otherwise it chooses
720p. Explicit targets bypass `skip-above` but never downscale.

## Build and verify

```sh
make                         # host-tuned plugin
make MARCH=x86-64 MULTIVERSION=1  # portable x86-64 build with runtime SIMD selection
make test                    # unit/contract tests with ASan and UBSan
make fuzz-smoke              # deterministic sanitizer fuzzing
make check                   # complexity plus tests
make analyze                 # complexity and static analysis
make scan-build              # Clang analyzer on plugin configurations
make coverage                # configured file/function coverage gates
make test-zimg               # zimg integration tests
make stress                  # USM concurrency stress
make stress-zimg             # zimg concurrency stress
make check-hardening         # linked-plugin hardening checks
make check-visibility        # exported-symbol check
```

The default `MARCH=native MULTIVERSION=0` build is for the build host. Use
`MARCH=x86-64 MULTIVERSION=1` when distributing to other Linux x86-64 systems.
The runtime dispatcher selects SSE2, AVX2, or AVX-512 USM code after loading.

Plugin builds require VLC and FFmpeg development packages. zimg tests require
zimg development files. `make fuzz` requires Clang/libFuzzer; analysis and
complexity targets require their named tools.

## Documentation

- [Usage and troubleshooting](docs/USAGE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Performance and benchmarking](docs/BENCHMARKS.md)
- [Cinnamon/Nemo integration](docs/DESKTOP_INTEGRATION.md)

## Compatibility

Supported: Linux x86-64, VLC 3.x, GCC or Clang. VLC 4, other operating systems,
and other architectures are outside the compatibility contract.

Useful diagnostics:

- `vlc-plugin pkg-config not found`: install the VLC development package.
- Plugin not listed: run `make info`, reinstall, then regenerate VLC's cache.
- Opaque/unsupported chroma: try `--avcodec-hw=none`.
- Playback cannot keep up: try `--autoupscale-algo=1`,
  `--autoupscale-usm=0`, or `--autoupscale-target=1`.
