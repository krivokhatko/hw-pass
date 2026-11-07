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

### StickS3

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
   Without a build: write the full image of a [release](https://github.com/krivokhatko/hw-pass/releases) ("Releases" in README.md):
   ```
   esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 hw-pass-<version>-sticks3.bin
   ```
5. The firmware starts and shows the list of entries, which is empty. If the device stays in download mode, push the power button one time.
6. Open the command line: `picocom /dev/ttyACM0`. Push Enter: the prompt shows.

After this, `tools/flash.sh` flashes the device without the power button. It sends `dfu` to the hw-pass serial port, waits for the download mode, and runs `idf.py flash`.

### Chain Dual Key

The Chain Dual Key comes with the firmware "Chain DualKey User Demo" (`303a:8000`). It does not read the serial port: esptool cannot reset the chip through it. The board has no reset button. Key1 is the boot strap pin:

1. Put the power switch in the middle position. Disconnect the cable.
2. Hold Key1, the key far from the lanyard hole. Connect the cable. Release Key1. The computer sees `303a:1001`: the chip is in the ROM download mode. On WSL2, bind and attach this identity too.
3. Save the factory firmware, if you want it back one day (8 MB, about 2 minutes):
   ```
   esptool.py --chip esp32s3 -p /dev/ttyACM0 read_flash 0 0x800000 dualkey-factory.bin
   ```
   M5Burner also has the image ("Chain DualKey User Demo").
4. Erase the flash, build with the board file, and flash:
   ```
   get_idf
   idf.py -p /dev/ttyACM0 erase-flash
   rm sdkconfig
   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/dualkey.defaults" build
   idf.py -p /dev/ttyACM0 flash
   ```
   Or write the full image of a release: `hw-pass-<version>-dualkey.bin` at `0x0`.
5. After this first flash, the chip stays in the download mode: the reset of esptool through the RTS line does not end a download mode that Key1 started. Start the firmware with a watchdog reset:
   ```
   esptool.py --chip esp32s3 -p /dev/ttyACM0 --before no_reset --after watchdog_reset --no-stub read_mem 0x60004038
   ```
   Or disconnect the cable and connect it again. The firmware starts: the left LED shows the link, the right LED is off (no entries). On WSL2, bind and attach the identity `303a:4005` too.

After this, `tools/flash.sh` works as on the StickS3. `sdkconfig` keeps the board: `idf.py build` builds the Chain Dual Key until you delete it.

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
2. Put the device in download mode (`dfu` command, or hold the power button; Chain Dual Key: hold Key1 while you connect the cable). Find `303a:1001` and bind it.

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

Without `sudo`, reset the USB device from user space. The udev rule of "Serial port access" gives the `plugdev` group access to the device node of the download mode (`303a:1001`):

```
python3 - <<'EOF'
import fcntl, os
dev = os.path.realpath("/sys/class/tty/ttyACM0/device/..")
bus, num = (int(open(f"{dev}/{f}").read()) for f in ("busnum", "devnum"))
fd = os.open(f"/dev/bus/usb/{bus:03d}/{num:03d}", os.O_WRONLY)
fcntl.ioctl(fd, 0x5514)  # USBDEVFS_RESET
os.close(fd)
EOF
```

The device keeps its port, its identity, and the download mode. This also helps when a long transfer stops, for example `read_flash` of the full 8 MB: read the flash in pieces of 1 MB, and reset the device after a piece that stops.

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
| Chain DualKey User Demo (factory firmware of the Chain Dual Key) | `303a:8000` | Chain DualKey |

## Download mode

Three ways put the chip in the ROM download mode:

| Way | Works with | Note |
|---|---|---|
| `dfu` on the hw-pass serial port | hw-pass | `tools/flash.sh` does this. |
| Hold the power button | StickS3, any firmware, also a device that does not start | The power chip resets the chip into the download mode. |
| Hold Key1 while you connect the cable | Chain Dual Key, any firmware | Key1 is the boot strap pin (G0). The power switch in the middle position. |
| `machine.bootloader()` in the UIFlow2 REPL | UIFlow2 | See below. |

If the device stays in download mode after a flash: StickS3, push the power button one time. Chain Dual Key, do the watchdog reset of step 5 in "Chain Dual Key", or disconnect the cable and connect it again.

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
