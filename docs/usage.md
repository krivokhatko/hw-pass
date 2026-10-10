# User guide

hw-pass is a USB keyboard that types stored passwords. Select an entry with the buttons. The device types the password into the focused window.

## Buttons

| Button | Short push | Long push (0.6 s) |
|---|---|---|
| Side (KEY2) | Select the next entry | Select the previous entry |
| Front (KEY1, below the screen) | Type the password | Type the password, then push Enter |
| Both together | - | Hold 2 s: start the Wi-Fi setup mode |

Push the two buttons at the same time. If you hold the front button alone for 0.6 s, the device types the password.

The screen goes dark after 60 s without activity. The first push turns it on again and does nothing else.

The power button on the other side belongs to the power chip:

- One push: power on or reset.
- Two pushes: power off.
- Long push: ROM download mode (for flashing).

## Screen

```
hw-pass        3/12  USB      <- position, USB state (green: ready)
  GitHub
  Mail                        <- the selected entry has a color bar
  Bank
------------------------------
Side: next  Front: type
```

## Wi-Fi setup mode

Use a phone or a laptop to add and change entries.

1. Hold the two buttons for 2 s. A bar on the screen shows the progress.
2. The screen shows a QR code, the network name, and the Wi-Fi password.
3. Phone: scan the QR code with the camera. The phone connects. An iPhone usually opens the page by itself. On Android, tap the "Sign in to Wi-Fi network" notification, or tap the network in the Wi-Fi settings. The page opens in the sign-in window.
   Laptop: connect to the network with the password, then open `http://192.168.4.1/`.
4. Change the entries. Then push "Done" on the page.

```
 ########   Wi-Fi       4:59  <- time until the mode stops
 # QR   #   hw-pass-7K3D      <- network name
 # code #   k7m2x9qd          <- Wi-Fi password, 2 lines
 #      #   4hrfpt8w
 ########   Scan the QR
            Button: stop
```

When a device connects, the QR code changes to the page address. The screen shows "Connected" and how to open the page.

Android sends the requests of a browser over mobile data while the Wi-Fi network has no internet. Then the browser cannot open `http://192.168.4.1/`, or shows `ERR_NETWORK_CHANGED`. The sign-in window uses the Wi-Fi network. To use a browser, turn off mobile data first.

The `web` command on the serial port also starts the mode. It shows the network name and the Wi-Fi password, so you can copy them.

The mode stops when one of these occurs:

- You push a button on the device.
- You push "Done" on the page.
- The page sends no request for 5 minutes.
- You type `web` again on the serial port.

During the stop, the screen shows "Stopping". The device ignores a button push until the list shows again.

The page:

| Part | Use |
|---|---|
| Entries | Push an entry to rename it, change its password, move it, or delete it. Delete needs two pushes. |
| Add an entry | Type the password two times, or make a random password of 8 to 127 characters. |
| Import | One entry on each line: `label,password`. See below. |
| Erase all entries | Type `YES` to confirm. |

Import rules:

- The password is all text after the first comma, also other commas.
- The device removes spaces around the label.
- The device refuses a password that starts or ends with a space, because this is usually a mistake (`label, password`). Use "Add an entry" for such a password.
- If one line is wrong, or the vault does not have space for all lines, the device adds no entries. The message shows the line number.
- The device writes all entries in one step. If the write fails, the vault keeps the old entries.

Like the command line, the page cannot show a password.

## Serial command line

The device is also a USB serial port (CDC-ACM). Use it to add and change entries. Open the port with any terminal program, for example:

- Linux: `picocom /dev/ttyACM0` (exit with `Ctrl+A Ctrl+X`).
- Windows: PuTTY, "Serial", the COM port of "USB Serial Device".

The baud rate has no effect. Push Enter to get the prompt.

| Command | Action |
|---|---|
| `help` | Show the commands. |
| `list` | Show the entries. |
| `add <label>` | Add an entry. Asks for the password two times. The password does not echo. |
| `gen [len] <label>` | Add an entry with a random password of 8 to 127 characters. The default length is 20. |
| `pass <n>` | Change the password of entry n. |
| `name <n> <label>` | Change the label of entry n. |
| `del <n>` | Delete entry n. |
| `move <n> <pos>` | Move entry n to position pos. |
| `wipe` | Erase all entries. |
| `kbtest` | Type all printable ASCII characters. Checks the key layout of the host. |
| `web` | Start the Wi-Fi setup mode and show the network name and the Wi-Fi password. If the mode is on, stop it. |
| `rotate` | Turn the screen 180 degrees. The device keeps the setting. |
| `info` | Show the firmware version and the device state. |
| `dfu` | Restart into ROM download mode for flashing. |

Limits:

- 32 entries.
- Label: 1 to 19 characters.
- Password: 1 to 127 characters.
- Only printable ASCII characters (space to `~`).

The command line cannot show a password. Only the front button can type it.

## Key layout

The device sends US key codes. On a host with a different key layout, some characters change (for example, `y` and `z` on a German layout). Use `kbtest` to check the host:

1. Put the focus on the terminal window.
2. Type `kbtest`.
3. The device types the test text, and the terminal sends it back. The device compares the text.

Caps Lock has no effect on the typed text. The device turns Caps Lock off before it types and turns it on again after. If the host does not turn Caps Lock off in 0.5 s, the device types nothing and shows "Caps Lock error". Turn Caps Lock off and try again.

## Security

Read these limits before you store important passwords:

- Flash encryption is off. A person with the device can read the passwords from the flash with esptool.
- There is no PIN. A person with the device can type all passwords.
- `del` and `pass` mark the old data as deleted, but the old bytes stay in flash until NVS reuses the page. `wipe` erases the full vault partition.
- `gen` and the random passwords of the page use the hardware random number generator. Its entropy source is the ADC noise when the radio is off, and the radio when Wi-Fi is on.

Wi-Fi:

- The radio is off, except in the Wi-Fi setup mode. A build with `CONFIG_HWPASS_WEB=n` has no Wi-Fi code.
- Each start makes a new network name and a new random Wi-Fi password of 16 characters (about 79 bits). The network uses WPA2/WPA3 and accepts one device. The transmit power is low (11 dBm).
- The device keeps no Wi-Fi data in flash.
- The page uses HTTP inside the encrypted Wi-Fi link. A person who has the Wi-Fi password while the mode is on can change and erase entries, but cannot read the passwords.
- The page accepts changes only with the session token and the host name `192.168.4.1`. Other web sites in the same browser cannot send changes.
- The firmware sets its copies of a password to zero after use. The Wi-Fi driver and the TCP/IP stack also keep the received bytes in their buffers until other data overwrites them. These buffers are in RAM only. A restart removes them.
