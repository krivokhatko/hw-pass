# hw-pass

Hardware password typer. Firmware for the M5Stack StickS3 (ESP32-S3).

The device is a USB keyboard. The side button selects a stored entry. The front button types its password. Two ways add and change entries: a command line on a USB serial port, and a web page in the Wi-Fi setup mode.

For the buttons, the commands, and the security limits, refer to [docs/usage.md](docs/usage.md).

## Target

| Item | Value |
|---|---|
| Board | M5Stack StickS3 |
| SoC | ESP32-S3-PICO-1, revision v0.2 |
| Flash | 8 MB, quad |
| PSRAM | 8 MB, not enabled yet |
| Console | USB Serial/JTAG. UART0 is not connected to USB. |

## Toolchain

- ESP-IDF v5.5.5, installed in `~/esp/esp-idf`.
- Do not use ESP-IDF v6.x yet. M5GFX has open display bugs on v6.x.

For the WSL setup, refer to [docs/dev-setup.md](docs/dev-setup.md).

## Layout

```
CMakeLists.txt        Project file.
sdkconfig.defaults    Project configuration. Edit this file, not sdkconfig.
partitions.csv        Flash partition table for 8 MB flash.
main/                 Application entry point (app_main). Keep it small.
components/           Project modules. One directory for each module.
docs/                 User and developer documentation.
tools/                Host scripts.
```

## Modules

| Module | Function |
|---|---|
| `pm1` | M5PM1 power chip: watchdog off, LCD power on, speaker off. |
| `display` | ST7789 LCD, backlight, frame buffer, and text. |
| `buttons` | Front and side buttons: debounce, short and long push, push of both. |
| `vault` | Password entries in the `vault` NVS partition. |
| `rng` | Random passwords. Selects the entropy source: ADC noise or radio. |
| `usbdev` | TinyUSB device: HID boot keyboard and CDC-ACM serial port. |
| `cli` | Command line on the USB serial port. |
| `web` | Wi-Fi setup mode: access point, captive DNS, web server, and the page (`www/index.html`). |
| `ui` | Screen content and button actions. |
| `sysinfo` | Chip information in the log. |

Threads: `ui_run` runs in the main task. `cli` has its own task. TinyUSB has its own task. In the Wi-Fi setup mode, the web server, the DNS server, and the Wi-Fi driver have their own tasks. `ui_run` starts and stops this mode. `vault`, `rng`, and the typing function in `usbdev` have locks.

## USB

The ROM and the first part of the start-up use the built-in USB Serial/JTAG (`303a:1001`). Then `usbdev` gives the USB PHY to TinyUSB, and the device connects again as `303a:4005` (HID + CDC). Log output goes to the CDC serial port.

`idf.py flash` cannot reset the chip through the TinyUSB serial port. Use the `dfu` command or `tools/flash.sh`.

## Build and flash

```
get_idf
idf.py build
tools/flash.sh
```

`tools/flash.sh` sends `dfu` to the hw-pass serial port, waits for the ROM download mode, and runs `idf.py flash`. After the flash, esptool resets the chip and the firmware starts.

If the device runs other firmware with USB Serial/JTAG, use `idf.py -p /dev/ttyACM0 flash`.

If the firmware does not start the serial port, push and hold the power button. The power chip starts the ROM download mode.

To open the command line: `picocom /dev/ttyACM0`. `idf.py monitor` also works, but it cannot show the start-up log.

Flashing does not erase the `vault` partition. `idf.py erase-flash` erases it.

## Configuration

`sdkconfig` is a generated file. Git ignores it. `sdkconfig.defaults` is the source of the configuration.

To change the configuration:

1. Add or change the option in `sdkconfig.defaults`.
2. Delete `sdkconfig`.
3. Run `idf.py build`.

Project options are in the "hw-pass" menu of `idf.py menuconfig`. For a build without Wi-Fi, add `CONFIG_HWPASS_WEB=n` to `sdkconfig.defaults`.

## Add a module

1. Make the directory `components/<name>/`.
2. Put the public headers in `components/<name>/include/`.
3. Register the module with `idf_component_register` in `components/<name>/CMakeLists.txt`.
4. Add `<name>` to `PRIV_REQUIRES` of each component that uses it.

The build includes only the components that `main` needs. A module that no component requires is not built.

For an example, refer to `components/sysinfo/`.

## External components

1. Add the component to the module that uses it: `idf.py add-dependency --component <module> "<namespace>/<name>^<version>"`.
2. Commit `components/<module>/idf_component.yml` and `dependencies.lock`.

Git ignores `managed_components/`.
