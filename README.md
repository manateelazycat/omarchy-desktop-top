# Omarchy Desktop Top

English | [简体中文](README.zh-CN.md)

A real, interactive btop terminal on the desktop, with the same angular frame, translucent background, corner marks, typography and theme colors as [Omarchy Desktop Clock](https://github.com/manateelazycat/omarchy-desktop-clock).

https://github.com/user-attachments/assets/897d885b-979f-4844-ae5e-6cfc77114b2a

[Original post](https://x.com/manateelazycat/status/2106797188567519713)

## Features

- Appears above the wallpaper on each monitor's empty workspace; ordinary, pinned and visible special-workspace windows hide it.
- Windows entirely outside the screen do not block an empty workspace; partially visible windows still hide the plugin.
- Drag the header or frame padding to move. Drag any of the four edges or four corners to resize, with directional cursors.
- Resizing changes the PTY's rows and columns, allowing btop to reflow without scaling its font. The default four panels require at least **80 columns × 24 rows**, plus the outer frame.
- Relative position and logical pixel size are shared across monitors, saved once on release and restored at login. Smaller displays clamp the card to available space.
- The top-right minimize button folds the window into a black top-edge tab with a theme-colored center line. Click the tab to restore the same position, size and running btop process. Minimized state is saved separately for each monitor and survives reloads and workspace changes.
- Near the top center, Desktop Clock uses the left tab slot and Desktop Top the right slot, with a 20-pixel gap. Windows and tabs appear only on empty workspaces.
- Click the terminal for keyboard input; mouse clicks and the wheel work in btop. It does not take keyboard focus before a click. After quitting btop with `q`, click the footer to restart.
- Isolated software rendering process. One btop process per empty monitor, kept running while minimized and stopped when its workspace becomes occupied; disabling the plugin stops all workers.
- Private temporary btop config and theme, based on the user's existing config, with transparent backgrounds, mouse input and one-second updates. The user's btop files are never rewritten.

## Security

The terminal passes btop's PTY output to libvterm. libvterm up to 0.3.3 grows its CSI argument index on every `;` or `:` without checking the array bound (`parser.c:229-235`), and btop tree view renders process argv without escaping control characters, so another local user can inject a sequence with more than 16 parameters and write past libvterm's argument array inside this process.

The plugin therefore clamps parameter separators at the PTY boundary itself (`native/CsiFilter.cpp`). It mirrors libvterm's ESC/CSI parsing in UTF-8 mode: a pending ESC survives BEL, other C0 controls, DEL and high bytes, and escape intermediates are tracked so a following `[` completes the escape instead of opening a CSI. Only the separators that would open a seventeenth parameter are dropped; text, UTF-8 and DCS/OSC payloads pass through byte-identically. This works regardless of the installed btop or libvterm versions and is exercised by `tests/csi_filter.test.cpp`, including against the real libvterm parser.

## Install

Requires Omarchy / Quickshell, Hyprland with Lua configuration, btop, Qt 6.5+ Quick/Qml, libvterm 0.3+, CMake, pkg-config and a C++ compiler. Arch packages: `quickshell btop qt6-declarative libvterm cmake pkgconf gcc`.

```sh
git clone https://github.com/manateelazycat/omarchy-desktop-top.git
cd omarchy-desktop-top
bash install.sh
```

The installer builds the native terminal module, stages the complete plugin in `~/.config/omarchy/plugins/io.github.manateelazycat.desktop-top/`, adds plugin-specific animation rules, reloads and validates Hyprland, and enables the plugin. Backups go to `~/.local/state/omarchy/desktop-top/backups/`. XDG configuration/state overrides are supported.

To update while preserving activation:

```sh
bash install.sh --no-enable
```

Rebuild with the installer after upgrading Qt or libvterm.

### Marketplace setup

The marketplace's generic installer clones the repository without building the terminal module or adding the Hyprland layer rules. After adding the plugin, explicitly run:

```sh
bash "${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/io.github.manateelazycat.desktop-top/install.sh" --no-enable
omarchy plugin enable io.github.manateelazycat.desktop-top
```

Setup preserves the installed source and Git metadata, so `omarchy plugin update` remains available. After an update, run the setup command again to rebuild the native module. Setup backs up user configuration before adding the scoped, guarded Hyprland include and does not require sudo.

### Remove

```sh
omarchy plugin remove io.github.manateelazycat.desktop-top
```

This unloads the plugin and removes its installed directory after confirmation. The guarded include in `~/.config/hypr/hyprland.lua` becomes inactive once the plugin is removed. To remove the include too, delete the block from `-- >>> omarchy-desktop-top >>>` through `-- <<< omarchy-desktop-top <<<`, then run `hyprctl reload` and `hyprctl configerrors`. Configuration backups remain in the state directory.

## Settings

Edit the existing plugin entry in `~/.config/omarchy/shell.json`:

```json
{
  "id": "io.github.manateelazycat.desktop-top",
  "positionX": 0.64,
  "positionY": 0.52,
  "width": 1000,
  "height": 650,
  "fontSize": 13,
  "opacity": 1,
  "boxes": "cpu mem net proc"
}
```

Positions range from 0–1 across available travel. Width and height are logical pixels; the frame adds 52 pixels horizontally and 98 vertically. Font size is 8–32; opacity is 0.2–1, applied to the whole card. Background alpha is 0.76, matching the clock.
`boxes` supports combinations of the four standard panels; the minimum follows btop's rules for these startup panels. Temporary panel toggles inside btop are not persisted to plugin settings. A monitor too small for the minimum card plus 24-pixel margins does not display it; reduce font size or the startup panels to fit.

```sh
omarchy-shell desktop-top status
omarchy-shell desktop-top setPosition 0.64 0.52
omarchy-shell desktop-top resetPosition
omarchy-shell desktop-top setSize 1000 650
omarchy-shell desktop-top resetSize
omarchy plugin disable io.github.manateelazycat.desktop-top
omarchy plugin enable io.github.manateelazycat.desktop-top
```

Status reports the worker, visible monitors, geometry, terminal rows/columns and btop PIDs. Disabling removes inline settings; installation backups retain the previous values.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
omarchy plugin validate .
g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/csi_filter.test tests/csi_filter.test.cpp native/CsiFilter.cpp && /tmp/csi_filter.test
g++ -std=c++17 -Wall -Wextra -Werror -DCSI_FILTER_WITH_VTERM -o /tmp/csi_filter-vterm.test tests/csi_filter.test.cpp native/CsiFilter.cpp $(pkg-config --cflags --libs vterm) && /tmp/csi_filter-vterm.test
node --test tests/geometry.test.cjs
bash tests/run-runtime.sh
python3 tests/bridge.py
python3 tests/install.py
```

Runtime tests use isolated processes and synthetic Qt input, without moving the system pointer or writing user settings. They cover real btop output, stable dragging, all resize edges, the minimum PTY, keyboard input, theme updates, persisted geometry, desktop occupancy and process cleanup. Installer tests use a temporary fixture to verify repeated marketplace setup preserves source and Git metadata.

## License

[GPL-3.0-only](LICENSE). Frame, desktop occupancy and host/worker IPC adapted from Omarchy Desktop Clock, Copyright (C) 2026 ManateeLazyCat.
