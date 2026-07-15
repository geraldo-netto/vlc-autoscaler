# Cinnamon and Nemo integration

The installer adds a separate AutoUpscale launcher and file action. It does not
modify VLC's standard desktop entry.

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
| `~/.local/share/nemo/actions/vlc-autoupscale.nemo_action` | Nemo file action |

Remove all three files with:

```sh
scripts/install-vlc-autoupscale-action.sh --uninstall
```

## Configure

The wrapper defaults to explicit 1080p, Spline36, and 20% USM. Replace its
entire option set with `VLC_AUTOUPSCALE_ARGS`:

```sh
VLC_AUTOUPSCALE_ARGS='--video-filter=autoupscale --autoupscale-target=1 --autoupscale-algo=1 --autoupscale-usm=0' vlc-autoupscale clip.mkv
```

The replacement must include `--video-filter=autoupscale`. An empty value
launches plain VLC.

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

Only install or edit action files you trust. Selected filenames are untrusted
input; pass them as quoted arguments rather than constructing shell commands.

For playback settings, see [Usage](USAGE.md).
