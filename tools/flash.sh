#!/bin/sh
# Flash the firmware. A board with USB (ESP32-S3): restart hw-pass into the
# ROM download mode first. A board with a USB serial bridge (StickC Plus):
# esptool resets the chip itself.
# Run "get_idf" first. Usage: tools/flash.sh [build-dir]
# The build directory is "build", or the one of "idf.py -B <dir>" for
# a second board (see "Add a board" in docs/contributing.md).
set -eu

BUILD_DIR="${1:-build}"

APP_ID="303a:4005"      # hw-pass firmware (TinyUSB)
ROM_ID="303a:1001"      # ROM download mode (USB Serial/JTAG)
# The USB serial bridge of the StickC Plus: FTDI FT232R (or a compatible
# chip) on some units, CH9102 on others.
BRIDGE_IDS="0403:6001 1a86:55d4"

# Print the first /dev/ttyACM* or /dev/ttyUSB* port of the USB device with
# the ID $1. A ttyACM port is the device itself; a ttyUSB port is one more
# level down, under the interface of the bridge chip.
find_port() {
    for tty in /sys/class/tty/ttyACM* /sys/class/tty/ttyUSB*; do
        [ -e "$tty" ] || continue
        for dev in "$tty/device/.." "$tty/device/../.."; do
            dev=$(readlink -f "$dev")
            [ -e "$dev/idVendor" ] || continue
            if [ "$(cat "$dev/idVendor"):$(cat "$dev/idProduct")" = "$1" ]; then
                echo "/dev/${tty##*/}"
                return 0
            fi
            break
        done
    done
    return 1
}

for id in $BRIDGE_IDS; do
    if port=$(find_port "$id"); then
        echo "Flash through the serial bridge $port."
        exec idf.py -B "$BUILD_DIR" -p "$port" flash
    fi
done

if port=$(find_port "$APP_ID"); then
    echo "Restart $port into download mode."
    stty -F "$port" raw -echo
    exec 3<>"$port"
    sleep 0.3
    # Ctrl+C first: it stops a command that waits for input.
    printf '\003dfu\r' >&3
    sleep 0.5
    exec 3<&-
fi

n=0
until port=$(find_port "$ROM_ID"); do
    n=$((n + 1))
    if [ "$n" -gt 40 ]; then
        echo "No device in download mode ($ROM_ID)." >&2
        echo "Hold the power button for download mode, then run this script again." >&2
        exit 1
    fi
    sleep 0.5
done
echo "Flash through $port."
idf.py -B "$BUILD_DIR" -p "$port" flash
