# Development environment (WSL2)

The StickS3 connects to Windows through USB. usbipd-win forwards the device to WSL2.

## One-time setup

These steps are complete on the current machine. Use them for a new machine.

1. Enable systemd. Add this to `/etc/wsl.conf`:
   ```
   [boot]
   systemd=true
   ```
2. Load the CDC-ACM driver at boot:
   ```
   echo cdc_acm | sudo tee /etc/modules-load.d/cdc_acm.conf
   ```
3. Give the `plugdev` group access to the JTAG interface. Put this line in `/etc/udev/rules.d/99-espressif-usb-jtag.rules`:
   ```
   SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", MODE="0660", GROUP="plugdev"
   ```
4. Add the user to the `dialout` group. This gives access to `/dev/ttyACM*`:
   ```
   sudo usermod -aG dialout $USER
   ```
5. Install the packages:
   ```
   sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv \
       cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
   ```
6. Install ESP-IDF:
   ```
   git clone --depth 1 --branch v5.5.5 --recurse-submodules --shallow-submodules \
       https://github.com/espressif/esp-idf.git ~/esp/esp-idf
   ~/esp/esp-idf/install.sh esp32s3
   ```
7. Add this alias to `~/.bashrc`:
   ```
   alias get_idf='. $HOME/esp/esp-idf/export.sh'
   ```
8. Restart WSL. In PowerShell, run `wsl --terminate <distro>`.

## Attach the device

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

## Enter download mode from UIFlow2

UIFlow2 uses a TinyUSB serial port. esptool cannot reset the chip through this port. Use the MicroPython REPL:

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
