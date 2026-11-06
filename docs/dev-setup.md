# Development environment

You need a computer with Linux, or Windows with WSL2. The project is developed and tested on WSL2 (Ubuntu). The steps for Linux are the same, without the USB forwarding.

- macOS: ESP-IDF supports it. The serial port is `/dev/cu.usbmodem*`. Not tested.
- Windows without WSL: ESP-IDF supports it, but `tools/flash.sh` is a shell script. Type `dfu` on the serial port, then run `idf.py -p COMx flash`. Not tested.

## Toolchain

1. Install the packages (Debian, Ubuntu):
   ```
   sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv \
       cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 picocom
   ```
2. Install ESP-IDF v5.5.5:
   ```
   git clone --depth 1 --branch v5.5.5 --recurse-submodules --shallow-submodules \
       https://github.com/espressif/esp-idf.git ~/esp/esp-idf
   ~/esp/esp-idf/install.sh esp32s3
   ```
3. Add this alias to `~/.bashrc`:
   ```
   alias get_idf='. $HOME/esp/esp-idf/export.sh'
   ```
4. Open a new shell. `get_idf`, then `idf.py --version` shows `v5.5.5`.

The project is tested with ESP-IDF v5.5.5. Other versions are not tested.

## Serial port access (Linux and WSL2)

1. Add the user to the `dialout` group. This gives access to `/dev/ttyACM*`:
   ```
   sudo usermod -aG dialout $USER
   ```
2. Give the `plugdev` group access to the JTAG interface of the download mode. Put this line in `/etc/udev/rules.d/99-espressif-usb-jtag.rules`:
   ```
   SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", MODE="0660", GROUP="plugdev"
   ```
3. Log out and in again. The groups apply to new sessions.

## First flash of a new device

The StickS3 comes with the UIFlow2 firmware. UIFlow2 uses a TinyUSB serial port, and esptool cannot reset the chip through it. Use the power button:

1. Connect the device with a USB-C cable. On WSL2, attach it (see below).
2. Hold the power button until the computer sees the USB device `303a:1001`, "USB JTAG/serial debug unit": the chip is in the ROM download mode. On WSL2, bind and attach this identity too.
3. Erase the flash. This removes UIFlow2 and its files:
   ```
   get_idf
   idf.py -p /dev/ttyACM0 erase-flash
   ```
4. Build and flash:
   ```
   idf.py build
   idf.py -p /dev/ttyACM0 flash
   ```
5. The firmware starts and shows the list of entries, which is empty. If the device stays in download mode, push the power button one time.
6. Open the command line: `picocom /dev/ttyACM0`. Push Enter: the prompt shows.

After this, `tools/flash.sh` flashes the device without the power button. It sends `dfu` to the hw-pass serial port, waits for the download mode, and runs `idf.py flash`.

## Windows with WSL2

The StickS3 connects to Windows through USB. [usbipd-win](https://github.com/dorssel/usbipd-win) forwards the device to WSL2.

### One-time setup

1. Enable systemd. Add this to `/etc/wsl.conf`:
   ```
   [boot]
   systemd=true
   ```
2. Load the CDC-ACM driver at boot:
   ```
   echo cdc_acm | sudo tee /etc/modules-load.d/cdc_acm.conf
   ```
3. Do the steps of "Toolchain" and "Serial port access".
4. Restart WSL. In PowerShell, run `wsl --terminate <distro>`.

### Attach the device

The device has two USB identities (see "USB identities"). Windows sees each identity as a different device. Bind both one time, in an administrator PowerShell:

1. Start the hw-pass firmware. Run `usbipd list` and find `303a:4005`. Run `usbipd bind --busid <BUSID>`.
2. Put the device in download mode (`dfu` command, or hold the power button). Find `303a:1001` and bind it.

Then run:

```
usbipd attach --wsl --busid <BUSID> --auto-attach
```

Keep the window open. Without `--auto-attach`, a disconnected device does not come back.

These events disconnect the device:

- A watchdog reset or a power cycle.
- A change between firmware that uses the built-in USB Serial/JTAG and firmware with its own USB stack (hw-pass, UIFlow2). This includes the `dfu` command and the start of hw-pass after flashing.

A reset through the RTS line does not disconnect the device.

### Device is attached, but no data

Symptom: the port opens, but esptool shows "No serial data received", and `dmesg` shows many `vhci_hcd: unlink` lines. Cause: the USB identity changed without a disconnect, and Windows still uses the old endpoint layout. Detach the device from the WSL side:

```
echo 0 | sudo tee /sys/devices/platform/vhci_hcd.0/detach
```

`0` is the vhci port. `/sys/devices/platform/vhci_hcd.0/status` shows the port of each attached device. usbipd restarts the device, and `--auto-attach` attaches it again after a few seconds. If this does not help, disconnect the USB cable and connect it again.

### Bluetooth

WSL2 has no Bluetooth of its own. For a test of the Bluetooth computer link from WSL2, attach a USB Bluetooth adapter with usbipd, and run BlueZ in WSL2 (`sudo apt-get install bluez`). The WSL2 kernel has no `uhid`: BlueZ cannot make an input device from the keyboard, but `bluetoothctl` can pair, and a program can read the HID service over GATT. For this, start the daemon as `bluetoothd -n -P hog`: with its own HID profile on, BlueZ hides the HID service from D-Bus. For a test as a keyboard, use a Linux or Windows computer with Bluetooth.

## Reset through DTR and RTS

The USB Serial/JTAG controller uses the DTR and RTS lines to reset the chip.

- When a program opens the port, Linux sets DTR=1 and RTS=1. This does not reset the chip.
- DTR=0 with RTS=1 resets the chip.
- `idf.py flash` and `idf.py monitor` use the correct sequence. Use them for normal work.

## USB identities

| Firmware | VID:PID | Product |
|---|---|---|
| ROM download mode, ESP-IDF app | `303a:1001` | USB JTAG/serial debug unit |
| hw-pass firmware (after start-up) | `303a:4005` | hw-pass password typer |
| UIFlow2 | `303a:832b` | StickS3(UiFlow2) |

## Download mode

Three ways put the chip in the ROM download mode:

| Way | Works with | Note |
|---|---|---|
| `dfu` on the hw-pass serial port | hw-pass | `tools/flash.sh` does this. |
| Hold the power button | Any firmware, also a device that does not start | The power chip resets the chip into the download mode. |
| `machine.bootloader()` in the UIFlow2 REPL | UIFlow2 | See below. |

If the device stays in download mode after a flash, push the power button one time.

### From the UIFlow2 REPL

1. Open the REPL on `/dev/ttyACM0`.
2. Push `Ctrl+C` to stop the running program.
3. Type `import machine; machine.bootloader()`.
4. Flash the new firmware.
5. Do a watchdog reset one time:
   ```
   esptool.py --chip esp32s3 -p /dev/ttyACM0 --before no_reset --after watchdog_reset --no-stub read_mem 0x60004038
   ```

Step 5 is necessary. After `machine.bootloader()`, the strap register shows GPIO0 low, and each RTS reset starts download mode again. A watchdog reset starts the application. The command prints the strap register before the reset. `0x00000000` means GPIO0 is low.

## Restore UIFlow2

The image is from the M5Burner catalog ("UIFlow2.0 StickS3", v2.5.3).
SHA-256: `c08196024019273ccadee9b4b3da303567f7ab206fa14edd1af283f04f6e98d8`.

```
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash --erase-all 0x0 uiflow2-sticks3-v2.5.3.bin
```
