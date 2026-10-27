# dialmenu

A radial menu for the Microsoft Surface Dial on Hyprland. Hold or click the dial to open the wheel under the pointer. Turn to move the highlight, then release to keep that tool. Turning while the wheel is closed runs the selected tool.

The menu follows the current Omarchy theme accent. Haptic clicks are sent to the dial when the wheel opens and each time the highlight moves.

## Build

Dependencies: GTK 4, gtk4-layer-shell, json-glib, libevdev, hidapi-hidraw, cairo, pango.

```
make
make install
systemctl --user enable --now dialmenu.service
```

`make install` copies the binary to `~/.local/bin` and the user service to `~/.config/systemd/user`.

Haptics and scroll injection need the session user to open the dial's hidraw node and `/dev/uinput`. Install `65-dialmenu.rules` into `/etc/udev/rules.d/` and reload udev. Load `uinput` if `/dev/uinput` is missing.

Keep the wheel's transparent corners from blurring:

```lua
hl.layer_rule({
  match = { namespace = "dialmenu" },
  blur = false,
  no_anim = true,
})
```

## Config

The menu lives at `~/.config/dialmenu/config.json`. It is created from the built-in default on first run. `config.example.json` shows the same shape. Saving the file reloads the running menu.

```
dialmenu list
dialmenu add LABEL ICON ROTATE [CLICK] [FOLDER]
dialmenu folder LABEL [PARENT]
dialmenu launch LABEL ICON COMMAND [FOLDER]
dialmenu remove LABEL [FOLDER]
```

`ROTATE` and `CLICK` are `volume`, `mute`, `scroll`, `zoom`, `undo`, `exit`, `brightness`, `media`, `play`, or `none`. A folder opens another wheel. Back is the top wedge. Releasing on a tool keeps that tool, including one inside a folder. A launch item runs its command and closes the wheel.

`menu_step` plus `menu_friction` is how far the dial turns before the highlight moves. `action_step` is how far it turns before the selected tool runs, and it ignores friction. `invert` reverses direction. `haptics` turns the click on or off.

Custom actions in the JSON file:

```json
"rotate": { "type": "command", "up": "notify-send louder", "down": "notify-send quieter" }
"click": { "type": "key", "run": "ctrl+z" }
```
