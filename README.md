# fw16-kbd-uleds

Framework Laptop 16 keyboard backlight bridge that exposes QMK-controlled modules as a standard UPower keyboard backlight.

This tool creates a virtual LED device via `/dev/uleds` named `framework::kbd_backlight`.
Brightness changes from the desktop environment are translated into direct QMK/VIA HID commands and applied to detected Framework 16 keyboard, numpad, and RGB macropad modules.

## Background

The Framework Laptop 16 keyboard, numpad, and macropad modules use QMK-based firmware for backlight control. 
By default, their backlight state is not exposed as a standard `org.freedesktop.UPower.KbdBacklight` device.

As a result, desktop environment tools (such as KDE PowerDevil, GNOME settings, and utilities like `brightnessctl`)
cannot detect or control the keyboard backlight out of the box.

This daemon bridges that gap entirely in userspace.

## Features

- **Automatic device discovery**: Automatically finds FW16 keyboard and input modules.
- **Bidirectional hardware synchronization**: Reads the current backlight level on startup and monitors for changes made via hardware shortcuts (e.g., `Fn + Space`), keeping the system UI and all modules in sync.
- **Runtime hotplug handling**: Uses `NETLINK_KOBJECT_UEVENT` to handle module changes at runtime.
- **Configurable logging**: Adjustable debug levels via environment variables.

> **KDE users:** Plasma PowerDevil only supports a single keyboard backlight device.
> To use KDE’s native brightness slider and shortcuts, `unified` mode is required.

## Requirements

- Linux kernel with `uleds` (userspace LED) and `hidraw` support
  (`uleds` is provided by the standard kernel and loaded automatically by the service).
- `libsystemd` (for native D-Bus synchronization).
- `systemd` (technically optional, only necessary if you want to run with the provided service unit).

## Installation

### AUR (Recommended for Arch Linux)
This package is available in the AUR as `fw16-kbd-uleds-git`.

### Manual Build
```bash
make
sudo make install PREFIX=/usr
````

### Removal

```bash
sudo systemctl disable --now fw16-kbd-uleds.service
sudo make uninstall
```

## Usage

Enable and start the service:

```bash
sudo systemctl enable --now fw16-kbd-uleds.service
```

The service unit automatically ensures the `uleds` kernel module is loaded.

### UPower Enumeration Notes

UPower only enumerates keyboard backlight devices at startup.
Because this daemon creates the virtual LED device at runtime, UPower must be restarted after the service is started for the backlight to be detected.

If the daemon is stopped and started again (for example after an upgrade or configuration change), UPower must be restarted again to re-enumerate the device.

```bash
sudo systemctl restart upower
```

Once UPower has detected the keyboard backlight device, KDE Plasma’s PowerDevil may need to be restarted once to notice the newly-added UPower device (it does not always discover it dynamically).

After that initial restart (if needed), subsequent restarts/upgrades of `fw16-kbd-uleds` that require restarting UPower generally do **not** also require restarting PowerDevil again. Other desktop environments may behave differently.

## Configuration

The daemon can be configured via command-line options, environment variables, or a configuration file.

### Options and Environment Variables

| CLI Option             | Environment Variable            | Description                                                      | Default   |
| :--------------------- | :------------------------------ | :--------------------------------------------------------------- | :-------- |
| `-m, --mode`           | `FW16_KBD_ULEDS_MODE`           | Operation mode: `unified` or `separate`                          | `unified` |
| `-v, --vid`            | `FW16_KBD_ULEDS_VID`            | Comma-separated VIDs or `VID:PID` (hex)                          | `32ac`    |
| `-b, --max-brightness` | `FW16_KBD_ULEDS_MAX_BRIGHTNESS` | Maximum brightness value                                         | `3`       |
| `-p, --poll-ms`        | `FW16_KBD_ULEDS_POLL_MS`        | Hardware polling interval in ms                                  | `1000`    |
| `-l, --list`           |                                 | List auto-discovered devices and exit                            |           |
|                        | `FW16_KBD_ULEDS_DEBUG`          | Debug level: `0` (Quiet), `1` (Info), `2` (Verbose), `3` (D-Bus) | `0`       |

### Operation Modes

The mode determines how detected modules are presented to the system:

* **`unified` (Default)**: Groups all modules under a single virtual LED device named `framework::kbd_backlight`. Brightness changes are synced across all detected modules.

    * **KDE/PowerDevil Note**: KDE's `plasma-powerdevil` only supports a single keyboard backlight device. If you want to use the native KDE brightness slider and shortcuts, you must use `unified` mode.
* **`separate`**: Creates individual virtual LED devices for different module types, allowing independent control:

    * `framework::kbd_backlight` (Keyboards)
    * `framework::numpad_backlight` (Numpad)
    * `framework::macropad_backlight` (Macropad)

  While this mode exposes all modules to the system, PowerDevil will likely only detect one of them (usually the last one alphabetically). Use this mode only if you plan to manage the modules via custom scripts or other tools.

### Hardware Synchronization

The daemon periodically polls the hardware for its current backlight level. This enables two important features:

1. **Startup persistence**: On startup, the daemon reads the current backlight level from the modules and initializes the virtual LED device with that value. This prevents the backlight from being reset to 0 when the service starts.
2. **Bi-directional sync**: If you change the backlight level using hardware shortcuts (such as `Fn + Space`), the daemon detects this change and:

    * Updates the virtual LED device in sysfs.
    * Notifies UPower (via D-Bus) and KDE Plasma's PowerDevil service so the UI slider and OSD immediately reflect the new brightness level.
    * In `unified` mode, propagates the change from the main keyboard to all other connected modules (for example, syncing the numpad).
    * In `separate` mode, each virtual device is polled and managed independently.

### Discrete Brightness Levels

The Framework 16 keyboard modules support four discrete brightness levels (Off, 33%, 67%, 100%).
To provide the best experience with desktop environments such as KDE, the daemon defaults to a `max_brightness` of `3`.
This results in four fixed steps in the UI, avoiding a continuous range slider that does not map cleanly to the hardware capability.

### Auto-Discovery and Target Overrides

By default, the daemon scans for the following Product IDs under the Framework Vendor ID (`32ac`):

* `0012`, `0018`, `0019`: Keyboards (ANSI, ISO, JIS)
* `0014`: Numpad
* `0013`: RGB Macropad

#### Overriding Targets

You can use the `--list` (or `-l`) flag to see which devices are currently auto-discovered and obtain a copy-pasteable configuration string.

Example discovery:

```bash
fw16-kbd-uleds --list
```

```
Auto-discovered devices:

  [1] 32ac:0012 (framework::kbd_backlight)
  [2] 32ac:0014 (framework::numpad_backlight)

To target these specifically, use:
  CLI:  -v 32ac:0012,32ac:0014
  Conf: FW16_KBD_ULEDS_VID=32ac:0012,32ac:0014
```

Use the `--vid` option or `FW16_KBD_ULEDS_VID` environment variable to customize which devices are controlled.
You can provide a comma-separated list of Vendor IDs or specific `VID:PID` pairs.

Example:

```env
# Only control the main keyboard and a specific macropad
FW16_KBD_ULEDS_VID=32ac:0012,32ac:0013
```

### Configuration File

The systemd service unit is configured to load an environment file from `/etc/fw16-kbd-uleds.conf`.
This allows users (including those who install via the AUR) to customize the behavior without modifying the service unit itself.

Example `/etc/fw16-kbd-uleds.conf`:

```env
FW16_KBD_ULEDS_DEBUG=1
FW16_KBD_ULEDS_MODE=unified
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

## License

MIT

## Acknowledgements

The QMK/VIA HID protocol implementation was derived from Framework's `qmk_hid` repository:
[https://github.com/FrameworkComputer/qmk_hid](https://github.com/FrameworkComputer/qmk_hid)
