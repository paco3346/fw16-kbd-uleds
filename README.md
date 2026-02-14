# fw16-kbd-uleds

Framework Laptop 16 keyboard backlight bridge for KDE/UPower.

This tool creates a virtual LED device via `/dev/uleds` named `framework::kbd_backlight`.
Brightness changes from the desktop environment are translated into calls to `qmk_hid` and applied to detected Framework 16 keyboard, numpad, and RGB macropad modules.

## Features

- **Auto-detection**: Automatically finds FW16 keyboard and input modules.
- **Hotplug Support**: Uses `NETLINK_KOBJECT_UEVENT` to handle module changes at runtime.
- **Debouncing**: Avoids excessive calls to `qmk_hid` during rapid brightness changes.
- **Configurable Logging**: Adjustable debug levels via environment variables.

## Requirements

- Linux kernel with `uleds` module support.
- `qmk_hid` installed at `/usr/bin/qmk_hid`.
- `systemd` (for the provided service unit).

## Installation

### AUR (Recommended for Arch Linux)
This package is available in the AUR as `fw16-kbd-uleds-git`.

### Manual Build
```bash
make
sudo make install PREFIX=/usr
```

## Usage

Enable and start the service:
```bash
sudo systemctl enable --now fw16-kbd-uleds.service
```
The service unit automatically ensures the `uleds` kernel module is loaded.

## Configuration

The daemon can be configured via command-line options, environment variables, or a configuration file.

### Options and Environment Variables

| CLI Option | Environment Variable | Description | Default |
|:---|:---|:---|:---|
| `-m, --mode` | `FW16_KBD_ULEDS_MODE` | Operation mode: `unified` or `separate` | `unified` |
| `-v, --vid` | `FW16_KBD_ULEDS_VID` | Comma-separated VIDs or `VID:PID` (hex) | `32ac` |
| `-d, --debounce-ms` | `FW16_KBD_ULEDS_DEBOUNCE_MS` | Debounce time in milliseconds | `180` |
| `-b, --max-brightness` | `FW16_KBD_ULEDS_MAX_BRIGHTNESS` | Maximum brightness value | `100` |
| `-l, --list` | | List auto-discovered devices and exit | |
| | `FW16_KBD_ULEDS_DEBUG` | Debug level: `0` (Quiet), `1` (Info), `2` (Verbose) | `0` |

### Operation Modes

The mode determines how detected modules are presented to the system:

- **`unified` (Default)**: Groups all modules under a single virtual LED device named `framework::kbd_backlight`. Brightness changes are synced across all detected modules.
  - **KDE/Powerdevil Note**: KDE's `plasma-powerdevil` only supports a single keyboard backlight device. If you want to use the native KDE brightness slider and shortcuts, you **must** use `unified` mode.
- **`separate`**: Creates individual virtual LED devices for different module types, allowing independent control:
  - `framework::kbd_backlight` (Keyboards)
  - `framework::numpad_backlight` (Numpad)
  - `framework::macropad_backlight` (Macropad)
  - While this mode exposes all modules to the system, Powerdevil will likely only "see" one of them (usually the main keyboard). Use this mode only if you plan to manage the modules via custom scripts or other tools.

### Auto-Discovery and Target Overrides

By default, the daemon scans for the following Product IDs under the Framework Vendor ID (`32ac`):
- `0012`, `0018`, `0019`: Keyboards (ANSI, ISO, JIS)
- `0014`: Numpad
- `0013`: RGB Macropad

#### Overriding Targets
You can use the `--list` (or `-l`) flag to see which devices are currently auto-discovered and get a copy-pasteable configuration string.

Example discovery:
```bash
fw16-kbd-uleds --list
```

Use the `--vid` option or `FW16_KBD_ULEDS_VID` environment variable to customize which devices are controlled. You can provide a comma-separated list of Vendor IDs or specific `VID:PID` pairs.

Example:
```env
# Only control the main keyboard and a specific macropad
FW16_KBD_ULEDS_VID=32ac:0012,32ac:0013
```

### Configuration File

The systemd service unit is configured to load an environment file from `/etc/fw16-kbd-uleds.conf`. This allows users (including those who install via the AUR) to customize the behavior without modifying the service unit itself.

Example `/etc/fw16-kbd-uleds.conf`:
```env
FW16_KBD_ULEDS_DEBUG=1
FW16_KBD_ULEDS_DEBOUNCE_MS=200
```

After modifying the configuration file, restart the service:
```bash
sudo systemctl restart fw16-kbd-uleds.service
```

### Systemd Drop-ins (Alternative)

Alternatively, you can use `systemctl edit` to create a drop-in:
```bash
sudo systemctl edit fw16-kbd-uleds.service
```
Add the following to the file:
```ini
[Service]
Environment=FW16_KBD_ULEDS_DEBUG=2
```

## Desktop Environment Integration

Because this daemon instantiates a virtual LED device via the `uleds` kernel module, `upower` may need to be restarted to enumerate the new `framework::kbd_backlight` class device. For KDE Plasma users, restarting the `plasma-powerdevil` service ensures that the desktop environment picks up the new backlight control.

```bash
sudo systemctl restart upower
systemctl --user restart plasma-powerdevil
```

## License

MIT
