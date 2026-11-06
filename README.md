# hw-pass

Hardware password typer. Firmware for the M5Stack StickS3 (ESP32-S3).

The device is a USB keyboard that types stored passwords. The side button selects an entry. The front button types its password into the focused window.

- Bluetooth LE keyboard and mouse: a paired laptop gets the keys without a cable. The laptop also sees the battery level of the device.
- Bluetooth LE keypad: each key types one entry. Enter and Backspace go to the computer. `-` and `+` select an entry, `*` turns the mouse mover on or off.
- Entries: a command line on the USB serial port, or a web page in the Wi-Fi setup mode. Encrypted backup and import.
- PIN: the entries are encrypted in flash, and the device types nothing until you enter the PIN with the buttons or the keypad.
- Layout key: before it types, the device can push a key that selects the key layout of the host.
- Mouse mover: the device is also a USB mouse that keeps the computer awake, by hand or on a schedule. The time comes from an NTP server over Wi-Fi, or you set it.
- A short sound tells the result of each action. The screen turns with the device.

| Document | Content |
|---|---|
| [docs/usage.md](docs/usage.md) | User guide: the buttons, the screen, each function, the commands, and the security limits. |
| [docs/dev-setup.md](docs/dev-setup.md) | Toolchain, serial port access, the first flash of a new device, and the USB link from WSL2. |
| [docs/contributing.md](docs/contributing.md) | Code and text style, the hardware map, how to test a change, and the commit message format. |

## Hardware

| Item | Note |
|---|---|
| M5Stack StickS3 | ESP32-S3-PICO-1 with 8 MB flash, a 240 × 135 LCD, two buttons, a speaker, a tilt sensor, a battery, and USB-C. No change to the hardware. |
| USB-C cable | A data cable, not a charge-only cable. |
| Bluetooth LE keypad (optional) | Tested: A4Tech FBK21C. Other keyboards with HID over GATT are not tested. |
| Computer with Bluetooth LE (optional) | For the wireless mode. Tested with Linux (BlueZ). |

## Quick start

1. Install the toolchain and give yourself access to the serial port: [docs/dev-setup.md](docs/dev-setup.md).
2. Build:
   ```
   get_idf
   idf.py build
   ```
3. Flash. A new device comes with UIFlow2: hold the power button until the computer sees the download mode, then run `idf.py -p /dev/ttyACM0 erase-flash` and `idf.py -p /dev/ttyACM0 flash`. A device with hw-pass: `tools/flash.sh`.
4. Open the command line with `picocom /dev/ttyACM0` and push Enter. Add an entry:
   ```
   add GitHub
   ```
   The device asks for the password two times.
5. Put the focus on a text editor and push the front button. The device types the password.

Then set a PIN (`pin`), add entries on the web page, pair a keypad, or pair a laptop. The user guide tells how: [docs/usage.md](docs/usage.md).

## Target

| Item | Value |
|---|---|
| Board | M5Stack StickS3 |
| SoC | ESP32-S3-PICO-1, revision v0.2 |
| Flash | 8 MB, quad |
| PSRAM | 8 MB, not enabled |
| Console | USB Serial/JTAG. UART0 is not connected to USB. |

## Toolchain

- ESP-IDF v5.5.5, installed in `~/esp/esp-idf`. The project is tested with this version only.
- Python and the tools of ESP-IDF. No other tools.

For the setup, refer to [docs/dev-setup.md](docs/dev-setup.md).

## Layout

```
CMakeLists.txt        Project file.
sdkconfig.defaults    Project configuration. Edit this file, not sdkconfig.
partitions.csv        Flash partition table for 8 MB flash.
main/                 Application entry point (app_main) and the project options (Kconfig.projbuild). Keep it small.
components/           Project modules. One directory for each module.
docs/                 User and developer documentation.
tools/                Host scripts: flash.sh flashes the device, psf2c.py makes the font file.
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

`main.c` starts the modules in this order: power chip, battery, display, sound, tilt sensor, vault, random numbers, clock, USB, Bluetooth host, Bluetooth computer link, keyboard output, mouse mover, keypad, time sync, command line. Then it runs the UI. A module that fails to start logs an error, and the device goes on: the serial port must stay available for the next flash.

Threads: `ui_run` runs in the main task. `cli` has its own task. TinyUSB has its own task. In the Wi-Fi setup mode, the web server, the DNS server, and the Wi-Fi driver have their own tasks. `ui_run` starts and stops this mode. `keypad` has its own task, and the NimBLE host has a task after the first Bluetooth start (`ble`). `blehid` has no task: the host task runs the events of the computer link. `sound` has its own task. The mouse mover, its schedule, and the battery measurement run in the `esp_timer` task. `ntp` has its own task; the Wi-Fi setup mode holds it while the mode runs. `vault`, `rng`, `hid`, `blehid`, `mover`, `ntp`, and the serial port in `usbdev` have locks.

Flash layout (`partitions.csv`): two 3 MB application slots for a later OTA update, the `nvs` partition for the settings and the bonding keys, and the `vault` partition for the entries. `wipe` erases the `vault` partition only.

## USB

The ROM and the first part of the start-up use the built-in USB Serial/JTAG (`303a:1001`). Then `usbdev` gives the USB PHY to TinyUSB, and the device connects again as `303a:4005`. Log output goes to the CDC serial port.

| Interface | Function | IN endpoint |
|---|---|---|
| 0 | HID boot keyboard | `0x81` |
| 1, 2 | CDC-ACM serial port | `0x82`, `0x83` |
| 3 | HID boot mouse | `0x84` |

The ESP32-S3 has 5 IN endpoints, EP0 included. All are in use: a new USB function needs a free IN endpoint. Add a new interface after the last one: then Windows keeps the COM port number. When the descriptors change, change `USBDEV_VERSION`.

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

The "Firmware" line of `info` shows the commit of the build (`git describe`), with `-dirty` when the tree had changes.

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

## Contributing

Changes are welcome. [docs/contributing.md](docs/contributing.md) has the rules: one module for one function, plain English in comments and documents, the documents change with the code, a commit message with one bullet for each module, and a test by hand on the device. The same document tells how to add a module, an external component, or a configuration option.

Before you store important passwords, read "Security" in [docs/usage.md](docs/usage.md): flash encryption is off, and the PIN protects the entries only within the limits that this section gives.

## Third-party code

| Code | Use | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.5 | Framework, drivers, NimBLE, mbedTLS, Wi-Fi | Apache-2.0 |
| [esp_tinyusb](https://components.espressif.com/components/espressif/esp_tinyusb) | USB device stack glue | Apache-2.0 |
| [TinyUSB](https://github.com/hathach/tinyusb) | USB device stack | MIT |
| [espressif/qrcode](https://components.espressif.com/components/espressif/qrcode) | QR code on the screen. Contains qrcodegen by Project Nayuki (MIT). | Apache-2.0 |
| [Terminus Font](https://terminus-font.sourceforge.net/) | `components/display/font.c` | SIL Open Font License 1.1, `components/display/OFL.txt` |
| Bosch BMI270 Sensor API | `components/imu/bmi270_config.c`, the configuration file of the sensor | BSD-3-Clause |

`idf.py build` downloads the Espressif components into `managed_components/`.
