# VLC AutoUpscale

VLC AutoUpscale is a Linux x86-64 video filter for VLC 3.x. It enlarges
low-resolution video in real time with zimg (preferred) or FFmpeg swscale,
then optionally sharpens luma with an unsharp-mask pass.

It is a classical resampler, not an AI upscaler. It improves presentation but
cannot recover detail absent from the source.

## Prepare the build environment

The plugin build requires a C compiler, GNU Make, pkg-config, VLC 3 development
headers, and FFmpeg's `libswscale`/`libavutil` development files. zimg is an
optional backend: install its development package for the preferred Spline36
path, or omit it for a swscale-only build.

Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential pkg-config libvlccore-dev libvlc-dev libswscale-dev libavutil-dev
# Optional preferred backend:
sudo apt install libzimg-dev
```

Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config vlc-devel ffmpeg-free-devel
# Optional preferred backend:
sudo dnf install zimg-devel
```

Confirm that the mandatory pkg-config modules are visible before building:

```sh
pkg-config --exists vlc-plugin libswscale libavutil
```

`make info` reports the detected compiler, plugin directory, CPU configuration,
and whether zimg is enabled.

## Build and install

```sh
make
sudo make install
```

If VLC does not find the installed plugin, rebuild the plugin cache at the path
shown by `make info`:

```sh
sudo vlc-cache-gen <vlc-plugins-directory>
```

### Optional verification environment

The normal `make` build does not need these tools. Install only the groups for
the verification targets you intend to run:

| Targets | Additional software |
|---|---|
| `make test`, `make fuzz-smoke`, `make stress` | GCC or Clang with ASan, UBSan, and TSan runtime support |
| `make fuzz` | Clang with libFuzzer support |
| `make check`, `make complexity` | Python 3 and Lizard |
| `make analyze` | Lizard, cppcheck, ShellCheck, and actionlint |
| `make scan-build` | Clang and `scan-build` (`clang-tools`) |
| `make coverage` | GCC, gcov, Python 3, gzip, and standard POSIX shell tools |
| `make test-zimg`, `make stress-zimg`, `make bench-zimg`, `make coverage-zimg` | Mandatory plugin dependencies plus zimg development files |
| `make check-hardening`, `make check-visibility`, `make check-multiversion-isa` | GNU binutils (`readelf`, `nm`, and `objdump`) |

On Debian or Ubuntu, prepare the complete local verification environment with:

```sh
sudo apt install ca-certificates curl tar clang clang-tools cppcheck shellcheck python3 python3-venv binutils gzip
python3 -m venv .venv
. .venv/bin/activate
python -m pip install lizard==1.17.31
```

Install the same checksum-verified actionlint binary used by CI:

```sh
mkdir -p "$HOME/.local/bin"
curl -sSLo actionlint.tar.gz \
  https://github.com/rhysd/actionlint/releases/download/v1.7.7/actionlint_1.7.7_linux_amd64.tar.gz
echo '023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757  actionlint.tar.gz' \
  | sha256sum -c -
tar -xzf actionlint.tar.gz -C "$HOME/.local/bin" actionlint
export PATH="$HOME/.local/bin:$PATH"
rm actionlint.tar.gz
```

The archive above is for Linux x86-64, matching this project's compatibility
contract. Keep the virtual environment active and verify the toolchain with:

```sh
command -v lizard cppcheck shellcheck actionlint scan-build gcov python3
```

Fedora users can install the packaged tools with `clang`, `clang-tools-extra`,
`cppcheck`, `ShellCheck`, `python3`, `python3-pip`, `binutils`, and `gzip`;
install Lizard in a virtual environment and actionlint from its verified
upstream release.

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
| `--autoupscale-threads` | 0–64 | 0 | Shared zimg/USM worker preference. `0` uses the automatic policy capped at 12 workers. |
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

The environment-preparation section above maps every target to its required
software. Missing optional tools do not affect a normal plugin build.

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
