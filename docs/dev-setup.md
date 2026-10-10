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

In PowerShell, run:

```
usbipd attach --wsl --busid <BUSID> --auto-attach
```

Keep the window open. Each chip reset disconnects the device from WSL. Without `--auto-attach`, the device does not come back.

## USB identities

| Firmware | VID:PID | Product |
|---|---|---|
| ROM download mode, ESP-IDF app | `303a:1001` | USB JTAG/serial debug unit |
| UIFlow2 | `303a:832b` | StickS3(UiFlow2) |

## Enter download mode from UIFlow2

UIFlow2 uses a TinyUSB serial port. esptool cannot reset the chip through this port. Use the MicroPython REPL:

1. Open the REPL on `/dev/ttyACM0`.
2. Push `Ctrl+C` to stop the running program.
3. Type `import machine; machine.bootloader()`.

## Restore UIFlow2

The image is from the M5Burner catalog ("UIFlow2.0 StickS3", v2.5.3).
SHA-256: `c08196024019273ccadee9b4b3da303567f7ab206fa14edd1af283f04f6e98d8`.

```
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash --erase-all 0x0 uiflow2-sticks3-v2.5.3.bin
```
