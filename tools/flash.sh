#!/bin/sh
# Restart hw-pass into ROM download mode, then flash the firmware.
# Run "get_idf" first. Usage: tools/flash.sh [build-dir]
# The build directory is "build", or the one of "idf.py -B <dir>" for
# a second board (see "Add a board" in docs/contributing.md).
set -eu

BUILD_DIR="${1:-build}"

APP_ID="303a:4005"  # hw-pass firmware (TinyUSB)
ROM_ID="303a:1001"  # ROM download mode (USB Serial/JTAG)

# Print the first /dev/ttyACM* port of the USB device with the ID $1.
find_port() {
    for tty in /sys/class/tty/ttyACM*; do
        [ -e "$tty" ] || continue
        dev=$(readlink -f "$tty/device/..")
        if [ "$(cat "$dev/idVendor"):$(cat "$dev/idProduct")" = "$1" ]; then
            echo "/dev/${tty##*/}"
            return 0
        fi
    done
    return 1
}

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
