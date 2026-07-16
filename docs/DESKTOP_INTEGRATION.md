# Cinnamon and Nemo integration

The installer adds a separate AutoUpscale launcher and Nemo file action. The
launcher uses VLC's low-overhead direct filter path. The Nemo action uses a
transcode display path so stock VLC 3 preserves the enlarged frame dimensions.
Neither modifies VLC's standard desktop entry.

## Install

Build and install the VLC plugin first, then run:

```sh
scripts/install-vlc-autoupscale-action.sh
```

The script locates VLC with `whereis`, validates the plugin when possible, and
installs:

| Path | Purpose |
|---|---|
| `~/.local/bin/vlc-autoupscale` | Wrapper with AutoUpscale defaults |
| `~/.local/share/applications/vlc-autoupscale.desktop` | Launcher and Open With entry |
| `~/.local/share/nemo/actions/vlc-autoupscale.nemo_action` | Forced-1080p Nemo action using the compatible transcode display path |

Remove all three files with:

```sh
scripts/install-vlc-autoupscale-action.sh --uninstall
```

## Launcher and Open With profile

The wrapper defaults to explicit 1080p, Spline36, and 20% USM. Replace its
entire option set with `VLC_AUTOUPSCALE_ARGS`:

```sh
VLC_AUTOUPSCALE_ARGS='--video-filter=autoupscale --autoupscale-target=1 --autoupscale-algo=1 --autoupscale-usm=0' vlc-autoupscale clip.mkv
```

The replacement must include `--video-filter=autoupscale`. An empty value
launches plain VLC.

## Nemo profile

The **Play with VLC (AutoUpscale 1080p)** action has a fixed profile:

- 1080p target, Spline36, and 20% USM;
- software decoding to avoid VLC 3 hardware-converter chain failures;
- a separate VLC instance so another player cannot absorb the action options;
- real-time x264 and AAC transcoding to preserve the enlarged dimensions at
  the display.

The transcode path costs more CPU than direct playback. The target also keeps
the plugin's 4x linear scaling cap: a 640x360 source reaches 1920x1080, while a
320x240 source reaches 1280x960. Re-running the installer restores this fixed
profile and overwrites local edits to the action file.

## Troubleshoot

If the action does not appear, restart Nemo:

```sh
nemo --quit
```

Inspect action parsing from a terminal:

```sh
nemo --quit
NEMO_DEBUG=Actions nemo --debug
```

Confirm the wrapper and plugin independently:

```sh
~/.local/bin/vlc-autoupscale path/to/video.mp4
vlc --list | grep autoupscale
```

The wrapper check exercises the direct path only. To verify the Nemo profile,
launch a video through the action and inspect VLC's messages for
`AutoUpscale engaged` and `destination (after video filters)`.

Only install or edit action files you trust. Selected filenames are untrusted
input; pass them as quoted arguments rather than constructing shell commands.

For playback settings, see [Usage](USAGE.md).
