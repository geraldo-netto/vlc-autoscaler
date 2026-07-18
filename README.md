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

`make info` reports the resolved compiler and library flags, install directory,
CPU configuration, and whether the zimg backend is enabled.

## Build and install

```sh
make
sudo make install
```

`make` builds the plugin and runs the ABI-layout and load-safe-ISA checks.
`make install` copies that artifact into the `video_filter` directory below
VLC's plugin root.

If VLC does not find the installed plugin, regenerate the cache from the plugin
root. Do not pass the `video_filter` install directory printed by `make info`:

```sh
plugin_root=$(pkg-config --variable=pluginsdir vlc-plugin)
sudo vlc-cache-gen "$plugin_root"
```

### Optional verification environment

The normal `make` build does not need these tools. Install only the groups for
the verification targets you intend to run:

| Targets | Additional software |
|---|---|
| `make test`, `make fuzz-smoke` | GCC or Clang with ASan and UBSan runtime support |
| `make stress` | GCC or Clang with ASan, UBSan, and TSan runtime support |
| `make fuzz` | Clang with libFuzzer support |
| `make check`, `make complexity` | Python 3 and Lizard |
| `make analyze` | Lizard, cppcheck, ShellCheck, actionlint, rumdl, and lychee |
| `make scan-build` | Clang and `scan-build` (`clang-tools`) |
| `make coverage` | GCC, gcov, Python 3, gzip, and standard POSIX shell tools |
| `make test-zimg`, `make fuzz-seam`, `make stress-zimg`, `make bench-zimg`, `make bench-pipeline`, `make coverage-zimg` | Mandatory plugin dependencies plus zimg development files |
| `make check-hardening`, `make check-visibility`, `make check-load-safe-isa`, `make check-multiversion-isa` | GNU binutils (`readelf`, `nm`, and `objdump`) |

On Debian or Ubuntu, prepare the complete local verification environment with:

```sh
sudo apt install ca-certificates curl tar clang clang-tools cppcheck shellcheck python3 python3-venv binutils gzip
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --only-binary=:all: --require-hashes \
  -r .github/requirements-ci.txt
```

The requirements file keeps the local Lizard version synchronized with CI.
Install the same checksum-verified actionlint, rumdl, and lychee binaries used
by CI:

```sh
mkdir -p "$HOME/.local/bin"
curl -sSLo actionlint.tar.gz \
  https://github.com/rhysd/actionlint/releases/download/v1.7.7/actionlint_1.7.7_linux_amd64.tar.gz
echo '023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757  actionlint.tar.gz' \
  | sha256sum -c -
tar -xzf actionlint.tar.gz -C "$HOME/.local/bin" actionlint
curl -sSLo rumdl.tar.gz \
  https://github.com/rvben/rumdl/releases/download/v0.2.34/rumdl-v0.2.34-x86_64-unknown-linux-gnu.tar.gz
echo '5fc1844544609504a5a8e9d37e1ef628a4a5938028dceda2b9c799173bf47739  rumdl.tar.gz' \
  | sha256sum -c -
tar -xzf rumdl.tar.gz -C "$HOME/.local/bin" rumdl
curl -sSLo lychee.tar.gz \
  https://github.com/lycheeverse/lychee/releases/download/lychee-v0.24.2/lychee-x86_64-unknown-linux-gnu.tar.gz
echo '1f4e0ef7f6554a6ed33dd7ac144fb2e1bbed98598e7af973042fc5cd43951c9a  lychee.tar.gz' \
  | sha256sum -c -
tar -xzf lychee.tar.gz -C "$HOME/.local/bin" \
  --strip-components=1 lychee-x86_64-unknown-linux-gnu/lychee
export PATH="$HOME/.local/bin:$PATH"
rm actionlint.tar.gz rumdl.tar.gz lychee.tar.gz
```

The archives above are for Linux x86-64, matching this project's compatibility
contract. Keep the virtual environment active and verify the toolchain with:

```sh
command -v lizard cppcheck shellcheck actionlint rumdl lychee \
  scan-build gcov python3
```

Fedora users can install the packaged tools with `clang`, `clang-tools-extra`,
`cppcheck`, `ShellCheck`, `python3`, `python3-pip`, `binutils`, and `gzip`;
install Lizard in a virtual environment and actionlint from its verified
upstream release. Use the verified rumdl and lychee releases above as well.

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
The recursion error is a stock VLC 3.0 converter-chain limit and cannot be
raised by an installed filter plugin. Use the transcode display path when you
need a forced-1080p result to reach the renderer:

```sh
vlc --avcodec-hw=none --autoupscale-target=2 --autoupscale-algo=3 --autoupscale-usm=20 --sout='#transcode{vcodec=h264,acodec=mp4a,vb=10000,ab=128,venc=x264{preset=ultrafast,tune=zerolatency},vfilter=autoupscale}:display' path/to/video.mp4
```

The Nemo installer exposes this compatibility profile through
`vlc-autoupscale --transcode-display`. The wrapper validates its required VLC
modules and trades additional real-time encode/decode work for reliable
enlarged display dimensions.

See [usage and troubleshooting](docs/USAGE.md) for practical variants.

## Options

| Option | Range | Default | Effect |
|---|---:|---:|---|
| `--autoupscale-target` | 0–6 | 0 | `0` AUTO; `1` 720p; `2` 1080p; `3` 1440p; `4` 4K; `5` 5K; `6` 8K. All targets have a 4× linear ratio cap. |
| `--autoupscale-algo` | 0–3 | 3 | `0` fast bilinear; `1` bicubic; `2` Lanczos; `3` Spline36. swscale maps Spline36 to Lanczos. |
| `--autoupscale-skip-above` | 0–8192 | 720 | In AUTO, skip sources at or above this height. `0` disables the gate. |
| `--autoupscale-usm` | 0–200 | 20 | Luma sharpening percentage. `0` disables USM. |
| `--autoupscale-backend` | 0–2 | 0 | `0` prefer zimg with swscale fallback; `1` zimg only; `2` swscale only. |
| `--autoupscale-threads` | 0–64 | 0 | Shared zimg/USM worker preference. `0` uses the automatic policy capped at 12 workers. |
| `--autoupscale-pin-threads` | 0–1 | 1 | Best-effort zimg worker pinning. Disable if it regresses the deployment host. |
| `--autoupscale-zerocopy-dst` | 0–1 | 1 | Direct zimg writes on compatible row grids. `0` forces copy-out. |
| `--autoupscale-zerocopy-src` | 0–1 | 1 | Direct zimg reads. `0` forces copy-in and disables column tiling. A resulting grid change can create bounded resampling seams, so copy and direct modes are byte-identical only when their grid is unchanged. |
| `--autoupscale-content-probe` | 0–1 | 1 | Emit one advisory for soft and blocky sources. It never changes output. |
| `--autoupscale-usm-stripe-min-rows` | 0–256 | 0 | Minimum USM rows per worker. `0` selects 8. |
| `--autoupscale-zimg-stripe-lines` | 0–128 | 0 | Advanced minimum zimg output lines per stripe. It can change the row/column grid and output seams; benchmark it with integration quality checks. `0` selects the validated default of 16. |
| `--autoupscale-usm-sharp-threshold` | 0–20000 | 3500 | Skip USM on grainy sources above this metric. `0` disables skipping. |

AUTO chooses 1080p only with at least four available CPUs and an upscale ratio
no greater than 4×. Otherwise it chooses 720p. Explicit targets bypass
`skip-above` but never downscale. AUTO never gates its choice on detected RAM;
allocation failures are handled by the normal backend failure path.

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
sudo make uninstall          # remove the installed plugin
```

The default `MARCH=native MULTIVERSION=0` build is for the build host. Use
`MARCH=x86-64 MULTIVERSION=1` when distributing to other Linux x86-64 systems.
`MARCH` controls the baseline of the plugin implementation; `MULTIVERSION=1`
only adds runtime-selected SSE2, AVX2, and AVX-512 variants for the USM kernel.
It does not lower the baseline of the scaler or VLC-facing implementation.

The VLC descriptor and guarded open callback are always built at the x86-64
baseline without LTO. For a v3 or v4 implementation, that callback verifies the
required CPU level before entering higher-ISA code. The default `make` and
explicit `make plugin` targets run `check-load-safe-isa` automatically. A
`MARCH=native` artifact is still host-specific; use the explicit `x86-64`
baseline for distribution.

The environment-preparation section above maps every target to its required
software. Missing optional tools do not affect a normal plugin build.

## Documentation

- [Usage and troubleshooting](docs/USAGE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Performance and benchmarking](docs/BENCHMARKS.md)
- [Cinnamon/Nemo integration](docs/DESKTOP_INTEGRATION.md)

## Compatibility

Supported: Linux x86-64 and VLC 3.x. `make test`, `make check`,
`make fuzz-smoke`, `make fuzz`, and `MULTIVERSION=1` require GCC 11+ or
Clang 12+ for `-march=x86-64-v3/v4`. VLC 4, other operating systems, and other
architectures are outside the compatibility contract.

Useful diagnostics:

- `vlc-plugin pkg-config not found`: install the VLC development package.
- Plugin not listed: run `make info`, reinstall, then regenerate VLC's cache.
- Opaque/unsupported chroma: try `--avcodec-hw=none`.
- Playback cannot keep up: try `--autoupscale-algo=1`,
  `--autoupscale-usm=0`, or `--autoupscale-target=1`.
