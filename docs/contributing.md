# Contributing

This document tells how the code is organized, how to write code and text in the style of the project, and how to test a change. Read [README.md](../README.md) first: it has the module list, the threads, the USB layout, and the configuration.

## Workflow

1. Build the current `main` branch and flash it. Check that the "Firmware" line of `info` on the serial port shows your commit. See [dev-setup.md](dev-setup.md).
2. Make a branch for your change.
3. Change the code. Update the documents in the same change (see "Documents").
4. Build without warnings, flash, and test by hand (see "Test a change").
5. Commit with a message in the project format (see "Commit messages").
6. Send a pull request, or send the patch.

A change should do one thing. Two unrelated changes go in two commits.

## Hardware map

The M5Stack StickS3 wires these parts to the ESP32-S3. The values are from the code.

| Part | Connection | Module |
|---|---|---|
| M5PM1 power chip | I2C, SDA G47, SCL G48, address 0x6E | `pm1` |
| BMI270 tilt sensor | Same I2C bus, address 0x69 | `imu` |
| ES8311 audio codec | Same I2C bus, address 0x18. I2S: MCLK G18, BCLK G17, WS G15, DOUT G14 | `sound` |
| AW8737 speaker amplifier | Enable through the power chip | `sound`, `pm1` |
| ST7789 LCD, 240 × 135 | SPI3: MOSI G39, SCLK G40, DC G45, CS G41, RST G21. Backlight PWM on G38 | `display` |
| Front button (KEY1) | G11, below the LCD | `buttons` |
| Side button (KEY2) | G12 | `buttons` |
| Power button | The power chip. One push resets, two pushes power off, a long push starts the ROM download mode. | - |
| Battery, charger | The power chip: voltage, USB power, and the charge state on its G0 | `pm1`, `battery` |
| USB-C | USB Serial/JTAG in the ROM, TinyUSB in the firmware | `usbdev` |

`pm1` owns the internal I2C bus. The other modules get the bus from it.

The USB vendor ID is the Espressif ID, and the product ID is the TinyUSB example ID for CDC and HID (`components/usbdev/include/usbdev.h`). The Bluetooth link reports the same IDs. For a product that you sell, get your own IDs.

## Code

### Layout of a module

One module is one directory in `components/`:

```
components/<name>/
    CMakeLists.txt      idf_component_register with SRCS, INCLUDE_DIRS "include", PRIV_REQUIRES
    include/<name>.h    The public interface. Each function has a comment.
    <name>.c            The code. Starts with a comment that tells what the module does and how.
```

Rules:

- A module has one function. `main.c` only starts the modules and runs the UI. Keep it small.
- A module exports only what another module needs. The rest is `static`.
- The public header has an `extern "C"` guard and the comments for the user of the module: what a function does, from which task to call it, and what it returns. The caller must not have to read the `.c` file.
- A module that two tasks call has a lock. The header says so ("safe to call from different tasks", or "call from one task only").
- A module logs with `ESP_LOGx(TAG, ...)` and `static const char *TAG = "<name>";`. A log line is a short sentence in the language of the documents, without a period.
- Functions return `esp_err_t` when they can fail. Use `ESP_RETURN_ON_ERROR` from `esp_check.h` for the early returns.

For an example of a small module, see `components/sysinfo/`. For a module with a timer and a callback, see `components/battery/`.

### Style

- 4 spaces, no tabs, LF, UTF-8, a newline at the end of the file (`.editorconfig`).
- Lines of about 100 columns. A long format string or a table can be longer.
- The brace of a function body is on its own line. The brace of `if`, `for`, `while`, and `switch` is on the same line. Always use braces, also for one statement.
- Names: `snake_case`. Static variables start with `s_`. Constants are `#define` in capitals, with a comment that says the unit and the reason:
  ```c
  #define IDLE_MS         1000    /* The speaker stays on for the next sound, also for a long push (0.6 s). */
  ```
- A constant has a name. No numbers in the code, except 0, 1, and array sizes.
- Declare the variables at the start of the block.
- Comments say what the code does and why. They do not say how, when the code shows it. A comment with a hardware fact names the source (datasheet, M5Unified, a Bluetooth specification).
- Secrets: set each copy of a password, a PIN, or a key to zero after use. Never log them. Never print them on the serial port or the page.
- Internal RAM is the limit, not flash. With Wi-Fi and Bluetooth on, about 90 KB of heap stays free. Prefer static buffers for data that lives long. Check the "Heap" line of `info` before and after your change.

### Language of comments and documents

Comments, log lines, screen texts, and the documents use plain English with the principles of ASD-STE100 (Simplified Technical English):

- Short sentences, about 20 words. One instruction or one fact in a sentence.
- Active voice, present tense. "The device locks when the computer disconnects." Not: "The device will be locked upon disconnection."
- Articles: "the keypad", "a computer". Not: "keypad disconnects".
- One name for one thing (see "Words"). Do not use a synonym for variation.
- Simple words: "use", not "utilize"; "start", not "initiate"; "tell", "show", "give".
- Numbers with a space before the unit: `20 s`, `30 ms`, `2 %`, `8 MB`.
- No "please", no "simply", no "just", no jokes. Say what to do and what occurs.
- A sentence with a colon gives the cause or the result: "No light sleep: USB needs the clocks."

Words:

| Word | Meaning |
|---|---|
| the device | hw-pass, the StickS3 with this firmware |
| entry | a stored label and password |
| label | the name of an entry |
| the PIN | the digits that unlock the device |
| the vault | the encrypted entries and their partition |
| the USB host | the computer on the USB cable |
| the computer | the computer that is paired over Bluetooth |
| the keypad | the Bluetooth LE keyboard that types entries |
| the link | USB or Bluetooth, the way that gets the keys |
| pair, pairing | the process that makes a bond |
| bond, bonding keys | the keys of a paired device, in NVS |
| the page | the web page of the Wi-Fi setup mode |
| the setup mode | the Wi-Fi setup mode |
| the mouse mover | the function that moves the pointer |
| the schedule | the times that turn the mouse mover on and off |
| the time sync | the NTP sync over Wi-Fi |
| the screen | the LCD and what it shows |
| the serial port, the command line | the CDC-ACM port and the commands on it |
| push, hold, release | what you do with a button or a key |
| type | what the device does with a password |

## Documents

The documents are part of the change. A pull request that changes what the user sees also changes the documents.

| Document | Content | Change it when |
|---|---|---|
| `README.md` | What the device does, the modules, the threads, the USB layout, the build, the configuration, the battery measures | A new module, a new task or lock, a new USB interface, a new configuration option, a change of the battery use |
| `docs/usage.md` | The user guide: the buttons, the screen, each function, each command, the limits, "Security" | A new or changed function, command, screen text, limit, or security property |
| `docs/dev-setup.md` | The toolchain, the USB link, download mode, the factory firmware | A change of the toolchain or the flash process |
| `docs/contributing.md` | This document | A change of a rule |

The `help` command and the command table in `docs/usage.md` list the same commands. `info` and its description in the table list the same items.

## Commit messages

```
Battery level for the Bluetooth computer

- pm1: read the battery voltage, the 5 V input voltage, the power
  source, and the status pin of the charger (PM1 G0).
- battery: new module. It measures every 20 s in the esp_timer task and
  gives the level in % from a discharge curve, in steps of 2 %.
- blehid: the Battery service reports the measured level, and the
  computer gets a notification at each change.
- cli: "info" shows the level, the voltage, USB power, and the charge
  state.
- README and docs/usage.md: the battery module and the level on the
  computer.
```

- The title is one line of up to 60 characters, without a period. It says what the change gives, as a noun phrase or in the imperative. A module name with a colon is fine: `Keypad: a click for Enter and Backspace`.
- An empty line, then the body. For a change in more than one module, one bullet for each module: the module name, a colon, what and why. For a change in one module, a short paragraph.
- Wrap the body at 72 columns. No trailers (`Signed-off-by`, `Co-authored-by`).
- The body says why, when the code does not show it. It does not repeat the diff.

## Test a change

The project has no automatic tests on the device. Test by hand. Each test needs a flashed device and the serial port (`picocom /dev/ttyACM0`).

Always:

1. `idf.py build` gives no warnings.
2. The start-up log has no `E (` lines. The device shows the list, and `info` works.
3. `info` shows a "Heap" line like the one before the change. A minimum below 80 KB needs a look.
4. Type an entry with the front button into a text editor on the USB host. `kbtest` shows no difference.
5. With a PIN: lock with `lock`, unlock with the buttons and with `unlock`. Pull the cable: the device locks.

For a change in a module, also test its function:

| Module | Test |
|---|---|
| `ui`, `buttons`, `display` | All screens: the list, the PIN screen, the Wi-Fi setup mode, the two pairings, a message. Turn the device: the screen follows. The screen goes dark and comes back. |
| `vault`, `kdf`, `backup`, `cli`, `web` | Add, change, move, delete, and import entries on the command line and on the page. Make a backup, decrypt it with `openssl`, import it again. Set, change, and remove the PIN. `wipe`. |
| `keypad`, `ble` | Pair the keypad. Give a key to an entry, type with it, remove the key. Enter and Backspace go to the computer. `-`, `+`, `*`. Let the keypad sleep: a key wakes it, and the device types. `unpair`. |
| `blehid`, `ble`, `hid` | Pair a computer. The keys go to the computer, and to the USB host after a disconnect. The device locks at the disconnect and accepts the bonded computer again. A pairing that the device did not start is refused. The battery level shows on the computer. `wireless forget`. |
| `usbdev`, `hid` | `kbtest` on Windows and on Linux. Caps Lock on before the typing: the text is correct. The mouse mover moves the pointer. Unplug and plug the cable. |
| `mover`, `clock`, `ntp` | `mouse`, `auto`, `time`, `tz`, `wifi`, `ntp`. A change of the time moves the schedule as "Time" in `docs/usage.md` says. |
| `sound` | Each sound in the table of `docs/usage.md`. A melody while the device types. `sound` off and on. |
| `pm1`, `battery` | `info` on USB and on battery. The screen goes dark after 20 s on battery. The level on the computer changes while the battery goes down. |
| `web`, `ntp` | Wi-Fi and Bluetooth at the same time: the setup mode with a connected keypad and a connected computer. The heap stays above 80 KB. |

For the Bluetooth computer link, a Linux computer with BlueZ shows the GATT services with `bluetoothctl` (`menu gatt`, `list-attributes`). `btmon` shows the packets. On Windows, the Device Manager shows the HID devices under the Bluetooth device.

### Logs

The serial port shows the log lines and the command line together. The level is Info. For debug lines of your module, set the level in the code while you test, and remove it before the commit:

```c
esp_log_level_set("keypad", ESP_LOG_DEBUG);
```

`CONFIG_LOG_MAXIMUM_LEVEL` in `sdkconfig` is Info: debug lines need `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` in `sdkconfig.defaults` for the test build. Do not commit it.

## Add a module

1. Make the directory `components/<name>/`.
2. Put the public header in `components/<name>/include/<name>.h`.
3. Register the module with `idf_component_register` in `components/<name>/CMakeLists.txt`.
4. Add `<name>` to `PRIV_REQUIRES` of each component that uses it, and to `main/CMakeLists.txt` when `main.c` starts it.
5. Add a row to the module table in `README.md`. If the module has a task or a lock, add it to the threads paragraph.

The build includes only the components that `main` needs (`MINIMAL_BUILD`). A module that no component requires is not built.

## External components

1. Add the component to the module that uses it: `idf.py add-dependency --component <module> "<namespace>/<name>^<version>"`.
2. Commit `components/<module>/idf_component.yml` and `dependencies.lock`.
3. Add the component and its license to "Third-party code" in `README.md`.

Git ignores `managed_components/`. `idf.py build` downloads them.

## Configuration options

- A build option for a function that a user can leave out goes in `main/Kconfig.projbuild` (`CONFIG_HWPASS_*`). The code around it uses `#if CONFIG_HWPASS_*`. A build with the option off must compile.
- A change of an ESP-IDF option goes in `sdkconfig.defaults`, with a comment that says why. Then delete `sdkconfig` and build again.
- Do not commit `sdkconfig`, `build/`, or `managed_components/`.

## Generated files

- `components/display/font.c`: `tools/psf2c.py` makes it from two Terminus console fonts, `Lat15-TerminusBold16.psf.gz` and `Lat15-TerminusBold24x12.psf.gz` (`/usr/share/consolefonts/`, package `console-setup-linux` on Debian and Ubuntu). The file names are in the file. Do not edit it by hand.
- `components/imu/bmi270_config.c`: the configuration file of the BMI270 from the Bosch Sensor API. Do not change it.
- `components/web/www/index.html`: the page. The build embeds it (`EMBED_FILES`). Edit the file and build again. The page has no build step and no external files: a phone without internet must load it.
