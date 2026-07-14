# Adding Custom Right-Click Actions to the Cinnamon Desktop

Cinnamon's desktop context menu is provided by Nemo. Custom entries are defined
with `.nemo_action` files in `~/.local/share/nemo/actions/`.

## Add an Action for the Desktop Background

Create the user action directory and a new action file:

```sh
mkdir -p ~/.local/share/nemo/actions
nano ~/.local/share/nemo/actions/open-terminal.nemo_action
```

Add this content:

```ini
[Nemo Action]
Name=Open terminal here
Comment=Open a terminal in the desktop directory
Exec=gnome-terminal --working-directory=%P
Icon-Name=utilities-terminal
Selection=none
Extensions=any;
Conditions=desktop;
Dependencies=gnome-terminal;
```

Save the file, then right-click an empty area of the desktop. Replace
`gnome-terminal` in `Exec` and `Dependencies` when using another terminal.

## Add an Action for Selected Desktop Icons

This action passes selected file and directory paths to a script:

```ini
[Nemo Action]
Name=Process selected files
Comment=Process the selected desktop items
Exec=/absolute/path/to/script.sh %F
Icon-Name=system-run
Selection=notnone
Extensions=any;
Conditions=desktop;
```

Make the script executable:

```sh
chmod +x /absolute/path/to/script.sh
```

Use an absolute script path. The script receives each selected path as a
separate argument.

## Add a "Play with VLC (AutoUpscale)" Action

The goal is a **second** VLC entry that launches with the plugin preconfigured,
side by side with the stock one. Nothing overwrites `vlc.desktop`: the new
launcher, the new "Open With" entry, and the Nemo action all live in separate
files with their own IDs, so the original VLC shortcut keeps working unchanged.

### Automatic install

```sh
scripts/install-vlc-autoupscale-action.sh
```

It installs three files under `$HOME` and warns if VLC does not list the
`autoupscale` module yet:

| File | Purpose |
| --- | --- |
| `~/.local/bin/vlc-autoupscale` | Wrapper that calls `vlc` with the filter flags. |
| `~/.local/share/applications/vlc-autoupscale.desktop` | Adds "VLC (AutoUpscale)" to the *Open With* menu and the app launcher. |
| `~/.local/share/nemo/actions/vlc-autoupscale.nemo_action` | Adds "Play with VLC (AutoUpscale)" to the right-click menu of video files. |

Remove everything again with:

```sh
scripts/install-vlc-autoupscale-action.sh --uninstall
```

### Manual install

Wrapper (`~/.local/bin/vlc-autoupscale`, `chmod +x`):

```sh
#!/bin/sh
set -eu
exec vlc --video-filter=autoupscale \
         --autoupscale-target=2 \
         --autoupscale-algo=3 \
         --autoupscale-usm=20 \
         -- "$@"
```

Nemo action (`~/.local/share/nemo/actions/vlc-autoupscale.nemo_action`):

```ini
[Nemo Action]
Name=Play with VLC (AutoUpscale)
Comment=Play the selected media with the autoupscale video filter enabled
Exec=/home/YOUR_USER/.local/bin/vlc-autoupscale %F
Icon-Name=vlc
Selection=notnone
Extensions=mp4;mkv;avi;mov;webm;m4v;ts;mpg;mpeg;wmv;flv;
Quote=double
Dependencies=vlc;
```

`Exec` must be an absolute path. `Quote=double` makes Nemo quote each selected
path, so filenames with spaces are passed as single arguments.

### Tuning

The wrapper honors `VLC_AUTOUPSCALE_ARGS`, which replaces the whole default flag
set:

```sh
VLC_AUTOUPSCALE_ARGS="--video-filter=autoupscale --autoupscale-target=4 --autoupscale-algo=2" \
    vlc-autoupscale clip.mkv
```

See [USAGE.md](USAGE.md) for the recipes and the full option table.

## Important Fields and Tokens

| Entry | Meaning |
| --- | --- |
| `Selection=none` | Show when no desktop icon is selected. |
| `Selection=s` | Show for one selected item. |
| `Selection=m` | Show for multiple selected items. |
| `Selection=notnone` | Show for one or more selected items. |
| `Extensions=any;` | Allow files and directories. |
| `Conditions=desktop;` | Restrict the action to the desktop. |
| `%P` | Path of the current directory. |
| `%F` | Full paths of selected items. |
| `%U` | URIs of selected items. |
| `%f` | Display name of the first selected item. |

Nemo requires `Name`, `Exec`, `Selection`, and either `Extensions` or
`Mimetypes` for a basic action.

## Reload and Debug

Nemo normally detects action-file changes automatically. If the entry does not
appear, quit Nemo so Cinnamon can restart it:

```sh
nemo --quit
```

This also closes open Nemo windows. To inspect action parsing errors, start Nemo
from a terminal with action debugging enabled:

```sh
nemo --quit
NEMO_DEBUG=Actions nemo --debug
```

## Safety

Only invoke trusted scripts. Avoid building shell commands by concatenating
selected filenames; treat every received path as untrusted input and pass it as
a quoted argument inside scripts.

## References

- [Nemo repository](https://github.com/linuxmint/nemo)
- [Official sample action and field reference](https://raw.githubusercontent.com/linuxmint/nemo/master/files/usr/share/nemo/actions/sample.nemo_action)
