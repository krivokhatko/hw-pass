# hw-pass

Hardware password typer. Firmware for the M5Stack StickS3 (ESP32-S3).

The device is a USB keyboard. The side button selects a stored entry. The front button types its password. The device is also a Bluetooth LE keyboard and mouse for a laptop without a cable: a paired computer gets the keys while it is connected. A paired Bluetooth LE keypad can also type: each key types one entry, and Enter and Backspace go to the computer. On the keypad, `-` and `+` select an entry, and `*` turns the mouse mover on or off. Two ways add and change entries: a command line on a USB serial port, and a web page in the Wi-Fi setup mode. A PIN encrypts the entries and locks the device until the user enters it, with the buttons or the keypad. Before it types, the device can push a key that selects the key layout of the host. The device is also a USB mouse: the mouse mover keeps the computer awake, by hand or on a schedule. The device gets the time from an NTP server over Wi-Fi, or you set it. A short sound on the speaker tells the result of each action. The screen turns with the device: a tilt sensor tells which edge is up.

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
main/                 Application entry point (app_main) and the project options (Kconfig.projbuild). Keep it small.
components/           Project modules. One directory for each module.
docs/                 User and developer documentation.
tools/                Host scripts.
```

## Modules

| Module | Function |
|---|---|
| `pm1` | M5PM1 power chip: watchdog off, LCD power on, speaker amplifier on and off, battery voltage, USB power and charge state. It owns the internal I2C bus. |
| `battery` | Battery of the device: the voltage from `pm1` every 20 s, and the level in % from a discharge curve. `blehid` reports the level to the computer; `info` shows it. |
| `display` | ST7789 LCD, backlight, panel sleep while the screen is dark, frame buffer, and text. |
| `sound` | ES8311 audio codec on I2S, and the AW8737 speaker amplifier: short tones and the typing melody for `ui`. |
| `clock` | Time in the system clock (UTC) and the time zone: kept across a restart, lost at a power loss. The board has no clock chip. |
| `imu` | BMI270 accelerometer on the internal I2C bus: which edge of the screen is up. The chip loads its 8 KB configuration file at each start (`bmi270_config.c`, from the Bosch BMI270 Sensor API, BSD-3-Clause). |
| `buttons` | Front and side buttons: debounce, short and long push, push of both. |
| `vault` | Password entries in the `vault` NVS partition. With a PIN, the entries are one AES-256-GCM blob: the key comes from PBKDF2 of the PIN, and from the eFuse HMAC key when the chip has one. The lock state, the wrong-PIN counter, and the wait times. |
| `kdf` | PBKDF2-HMAC-SHA512 on the SHA hardware, for `vault` and `backup`. |
| `rng` | Random passwords. Selects the entropy source: ADC noise or radio. |
| `usbdev` | TinyUSB device: HID boot keyboard, CDC-ACM serial port, and HID boot mouse. Sends the reports of `hid`. |
| `hid` | Keyboard and mouse output: types text, holds keys, and moves the pointer of the mouse mover. Selects the link: the Bluetooth computer when it is connected, else the USB host. The layout key and the Caps Lock check. |
| `mover` | Mouse mover: the on/off setting, and the schedule that turns it on and off at set times. |
| `ntp` | Time sync: a Wi-Fi station joins the network for some seconds and gets the time from an NTP server (SNTP, RFC 4330). |
| `cli` | Command line on the USB serial port. |
| `web` | Wi-Fi setup mode: access point, captive DNS, web server, and the page (`www/index.html`). |
| `backup` | Encrypted backup of the entries. The file format is the same as `openssl enc`. |
| `ble` | Bluetooth LE host (NimBLE), shared by `keypad` and `blehid`: the start, the pairing method, and the bonding keys in NVS. |
| `blehid` | Bluetooth LE keyboard and mouse for a computer (HID over GATT): the services, the advertising, the pairing with a passkey, the key and mouse reports, and the battery level. One paired computer in NVS. |
| `keypad` | Bluetooth LE keypad: NimBLE central, pairing, HID over GATT client, battery level, and key changes for `ui`. `hidmap.c` reads the HID report map. |
| `ui` | Screen content and button actions. The PIN screen, and the lock when the USB connection stops. |
| `sysinfo` | Chip information in the log. |

Threads: `ui_run` runs in the main task. `cli` has its own task. TinyUSB has its own task. In the Wi-Fi setup mode, the web server, the DNS server, and the Wi-Fi driver have their own tasks. `ui_run` starts and stops this mode. `keypad` has its own task, and the NimBLE host has a task after the first Bluetooth start (`ble`). `blehid` has no task: the host task runs the events of the computer link. `sound` has its own task. The mouse mover, its schedule, and the battery measurement run in the `esp_timer` task. `ntp` has its own task; the Wi-Fi setup mode holds it while the mode runs. `vault`, `rng`, `hid`, `blehid`, `mover`, `ntp`, and the serial port in `usbdev` have locks.

## USB

The ROM and the first part of the start-up use the built-in USB Serial/JTAG (`303a:1001`). Then `usbdev` gives the USB PHY to TinyUSB, and the device connects again as `303a:4005`. Log output goes to the CDC serial port.

| Interface | Function | IN endpoint |
|---|---|---|
| 0 | HID boot keyboard | `0x81` |
| 1, 2 | CDC-ACM serial port | `0x82`, `0x83` |
| 3 | HID boot mouse | `0x84` |

The ESP32-S3 has 5 IN endpoints, EP0 included. All are in use: a new USB function needs a free IN endpoint. Add a new interface after the last one: then Windows keeps the COM port number. When the descriptors change, change `USB_BCD`.

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

Project options are in the "hw-pass" menu of `idf.py menuconfig` (`main/Kconfig.projbuild`). For a build without the web page, add `CONFIG_HWPASS_WEB=n` to `sdkconfig.defaults`. For a build without the time sync, add `CONFIG_HWPASS_NTP=n`. With both, the build has no Wi-Fi code. For a build without Bluetooth, add `CONFIG_BT_ENABLED=n`: then the device cannot pair a keypad or a computer.

Wi-Fi and Bluetooth can run at the same time. Both need much internal RAM: the Wi-Fi and NimBLE options in `sdkconfig.defaults` make the buffers small. With both on, about 90 KB of heap stays free.

## Battery use

The device has no current measurement. These measures keep the use low on battery:

- CPU: `CONFIG_PM_ENABLE` and `esp_pm_configure` in `main.c`. The CPU runs at 80 MHz while all tasks wait, and at 160 MHz while a task runs. The APB clock stays at 80 MHz: the peripherals see no change. No light sleep: USB, the backlight PWM, and the Bluetooth controller would need more work.
- Screen: the backlight is the largest user. The screen goes dark after 60 s on USB power, after 20 s on battery (`ui`). While it is dark, the panel sleeps (`display_backlight(0)`).
- Keypad link (`keypad`): the device scans for the sleeping keypad 10 % of the time (30 ms in 300 ms), and uses a connection interval of 30 ms.
- Computer link (`blehid`): after the encryption, the device asks for a connection interval of 15 to 30 ms with a latency of 30 events. The computer can refuse. Key reports go at the next event in both cases.
- Advertising for the paired computer: 100 to 150 ms. A slower rate would save little and make the connection of the computer slow.
- Tilt sensor: low-power mode, 25 Hz, read every 100 ms (`imu`). Sound: the amplifier and the I2S clocks are off 1 s after a sound (`sound`). Wi-Fi: on only in the setup mode (5 min idle limit) and during a time sync.

Not done: light sleep between Bluetooth events (the largest possible gain), standby of the audio codec between sounds (adds 50 ms and a click risk), and USB off without USB power (TinyUSB restart).

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
