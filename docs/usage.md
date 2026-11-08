# User guide

hw-pass is a USB keyboard that types stored passwords. Select an entry with the buttons. The device types the password into the focused window. The device is also a Bluetooth keyboard for a computer without a cable. See "Bluetooth computer".

The firmware runs on four boards. This guide describes the M5Stack StickS3, with a screen and a speaker. The M5Stack StickC Plus has the same screen, a buzzer, and no USB keyboard: it types over Bluetooth only. The M5Stack Chain Dual Key has two keys and two LEDs instead of the screen, and the M5Stack StampS3A has one button and one LED: "StickC Plus", "Chain Dual Key", and "StampS3A" tell what differs. All commands of the serial port, the Wi-Fi setup mode, the keypad, the Bluetooth computer, and the mouse mover work the same on all boards.

## Buttons

| Button | Short push | Long push (0.6 s) |
|---|---|---|
| Side (KEY2) | Select the next entry | Select the previous entry |
| Front (KEY1, below the screen) | Type the password | Type the password, then push Enter |
| Both together | - | Hold 2 s, then release: start the Wi-Fi setup mode. Hold 5 s, then release: pair a Bluetooth keypad. Hold 8 s, then release: pair a computer over Bluetooth. |

Push the two buttons at the same time. If you hold the front button alone for 0.6 s, the device types the password. The footer shows what occurs when you release the two buttons. A beep sounds after 2 s, 5 s, and 8 s.

On the PIN screen, the side button moves the cursor and the front button picks an item. See "PIN".

The screen goes dark after 60 s without activity, or after 20 s on battery. The first push turns it on again and does nothing else.

The power button on the other side belongs to the power chip:

- One push: power on or reset.
- Two pushes: power off.
- Long push: ROM download mode (for flashing).

## Screen

```
hw-pass   3/12  MM BT 85% USB  <- position, mouse mover, keypad state and battery, link to the computer
  GitHub                 1     <- the keypad key of the entry
  Mail                         <- the selected entry has a color bar
  Bank                   2
-------------------------------
Side: next  Front: type  21:45 <- the time
```

"BT" shows only when a keypad is paired. Green: the keypad is connected. Gray: the device waits for the keypad. The number after "BT" is the battery level of the keypad. See "Battery level".

"USB" or "BLE" at the right is the link that gets the keys. Green: the computer is ready. "BLE" in gray: a computer is paired over Bluetooth, but it is not connected. "USB" in red: no computer is ready. See "Bluetooth computer".

"MM" shows only when the mouse mover is on. Green: a computer is ready. Gray: the pointer does not move.

The footer shows the time when it is set. "--:--" means: the schedule of the mouse mover is on, but the time is not set. See "Time".

With a PIN, the screen shows "Enter the PIN" and a row of digits until you enter the PIN. See "PIN".

### Orientation

The screen turns with the device. Hold the device with a long edge up: the text is upright, with the header at the top. Hold it with an end up: the text reads from the top to the bottom. The screen turns 0.5 s after the device.

When the device lies flat, the tilt sensor cannot tell which edge is up. Then the screen keeps its orientation, and `rotate` on the serial port turns it. A turn with `rotate` holds until you turn the device. The device keeps the orientation across a restart.

## Sounds

The device makes a short sound after each action, and plays a melody while it types. Thus you do not have to look at the screen, for example when you type with the keypad.

| Sound | Meaning |
|---|---|
| Click | The selection changed, a PIN digit went in, or Enter or Backspace went to the computer. |
| One beep | A hold reached the next step: release now. Or you must do the next step: type the passkey. |
| Melody | The device types the password. The melody starts at once, plays while the device types, and ends with the typing. Each entry has its own melody, from its label: you hear which entry types. |
| Three notes, up | An action is complete: a key got an entry, a keypad is paired, or a phone or laptop connected to the Wi-Fi network. |
| Two notes, up | A function is on: the mouse mover (also by its schedule), the Wi-Fi setup mode, or the sounds. |
| Two notes, down | A function is off (also the mouse mover by its schedule), a pairing stopped, or a key lost its entry. |
| Two low beeps | Error. The screen shows the cause. A keypad key without an entry also makes this sound. |

To turn the sounds on or off, type `sound` on the serial port. The device keeps the setting. The default is on.

## StickC Plus

The StickC Plus has the screen and the two buttons of the StickS3, a buzzer instead of the speaker, and an older chip, the ESP32. The ESP32 has no USB: the device cannot be a USB keyboard. It types over Bluetooth only: pair a computer first (see "Bluetooth computer"). The USB-C port charges the battery and gives the serial port, through a bridge chip in the device: on Linux it is `/dev/ttyUSB0` (`picocom -b 115200 /dev/ttyUSB0`).

What differs from the StickS3:

- Button A, below the screen, is the front button. Button B, on the side, is the side button. The pushes are the same.
- The header shows "BLE" always. Red: no computer is paired. Gray: the computer is not connected. Green: the computer gets the keys. "No computer" with "Pair a computer" tells that the device cannot type.
- The buzzer plays the sounds and the melody of the table in "Sounds" as square waves: the same notes, a harder tone.
- The power button on the side, under button B: one push turns the device on. Hold it 4 s: the device turns off. The chip has no download mode by this button: flashing needs none (see "First flash of a new device" in [dev-setup.md](dev-setup.md)).
- The PIN check takes about 5 s: the hash unit of the ESP32 is slower, and it hashes two times more blocks (see "PIN").
- The chip has no eFuse HMAC key: the key of the entries comes from the PIN alone (see "Hardware key").
- The device locks when the Bluetooth computer disconnects, after a start, and with `lock`. There is no USB connection that can stop.
- `info` has no "USB host" line. "Battery" shows "USB power" while the cable is in.
- The Bluetooth link to the computer wakes the radio about 7 times a second, not once a second as on the StickS3: the ESP32 sends keys only when it wakes. The typing speed is the same, about 25 ms for each key.

## Chain Dual Key

The Chain Dual Key has two mechanical keys with an RGB LED under each key, a battery, and a power switch. It has no screen and no speaker. Hold it with the power switch toward you: the lanyard hole is at the left.

| Key | Short push | Long push (0.6 s) |
|---|---|---|
| Key2, left, next to the lanyard hole ("side") | Select the next entry | Select the previous entry |
| Key1, right, far from the lanyard hole ("front") | Type the password | Type the password, then push Enter |
| Both together | - | As on the StickS3: hold 2 s, then release: Wi-Fi setup mode. 5 s: pair a keypad. 8 s: pair a computer. |

The LEDs show the state:

| LED | Color | Meaning |
|---|---|---|
| Left (Key2) | Green | The USB host gets the keys. |
| Left | Blue | The Bluetooth computer gets the keys. |
| Left | Blue, blinks | A computer is paired, but not connected. |
| Left | Red | No computer is ready. |
| Right (Key1) | A color | The selected entry. The entries have eight colors in the order of the list: amber, yellow, green, cyan, blue, purple, pink, white. Entry 9 has the color of entry 1 again. |
| Right | Off | No entries. |
| Right | Amber, blinks | The device is locked. Enter the PIN. |
| Right | Red, blinks | Wrong PINs. Wait before the next try. |
| Both | Amber, gets brighter | You hold both keys. After 2 s, the color changes to cyan: release now for the Wi-Fi setup mode. After 5 s, blue: release to pair a keypad. After 8 s, purple: release to pair a computer. |
| Both | Green | An action is complete: the password is typed, a key got an entry, a pairing is complete. |
| Both | Red | Error: no computer, wrong PIN, typing failed. The serial port shows the cause. |
| Both | Cyan, blinks | Wi-Fi setup mode. Steady cyan: a phone or a computer is on the network. |
| Both | Blue, blinks | Pairing of a keypad. Steady blue: type the passkey on the keypad. |
| Both | Purple, blinks | Pairing of a computer. Steady purple: type the passkey on the computer. |
| Both | Dim | No activity for 60 s, or 20 s on battery. A push makes them bright again and does nothing else. |

Texts go to the serial port (`picocom /dev/ttyACM0`): the passkey of a pairing, the name and the password of the Wi-Fi network, the result of an action. For the Wi-Fi setup mode and the pairing of a computer, the serial port is also the start: `web` and `wireless pair`. Both work without the keys.

PIN with the keys: push Key2 as many times as the digit. For 0, push it ten times. Then push Key1: the digit goes in. The left LED gives a white tick at each push, both LEDs at each digit. When all digits are in, hold Key1 for 0.6 s: the device checks the PIN. Green: unlocked. Red: wrong. Hold Key2 for 0.6 s: the last digit goes out. The keypad and `unlock` on the serial port also enter the PIN.

The power switch has three positions. "BLE" and "USB": the device is on, also on battery. The position does not change where the keys go: the Bluetooth computer gets them while it is connected, else the USB host. Middle: the device is off on battery and on with USB power. The board has no reset button: disconnect the cable and connect it again. `info` shows the battery level, the USB power, and the charge state.

To flash the board, see "First flash of a new device" in [dev-setup.md](dev-setup.md).

## StampS3A

The StampS3A is a module with one button and one RGB LED next to the USB-C port. It has no screen, no speaker, and no battery: it runs on USB power. The one button does all:

| Push | Action |
|---|---|
| Tap | Type the password of the selected entry. |
| Hold 0.6 s, then release | Select the next entry. After the last entry comes the first. The LED shows the color of the entry. |
| Hold 2 s, then release | Wi-Fi setup mode. |
| Hold 5 s, then release | Pair a keypad. |
| Hold 8 s, then release | Pair a computer over Bluetooth. |

After 0.6 s the LED turns amber and gets brighter: release now for the next entry. After 2 s it turns cyan, after 5 s blue, after 8 s purple: the color of the mode that a release starts. There is no "type, then Enter" and no "previous entry" on this board. A tap by accident types the password: keep the focus on a safe window, or set a PIN.

The LED shows the state:

| Color | Meaning |
|---|---|
| A color | The selected entry, and a computer gets the keys. The entries have eight colors in the order of the list: amber, yellow, green, cyan, blue, purple, pink, white. Entry 9 has the color of entry 1 again. |
| A color, with a blue tick every 2 s | The same, and the Bluetooth computer gets the keys, not the USB host. |
| Green | No entries. The USB host gets the keys. |
| Blue | No entries. The Bluetooth computer gets the keys. |
| Blue, blinks | A computer is paired, but not connected, and no USB host is ready. |
| Red | No computer is ready. |
| Amber, blinks | The device is locked. Enter the PIN. |
| Red, blinks | Wrong PINs. Wait before the next try. |
| Amber, gets brighter | You hold the button. After 2 s cyan, after 5 s blue, after 8 s purple: see the table above. |
| Green, steady, after an action | An action is complete: the password is typed, a key got an entry, a pairing is complete. |
| Red, steady, after an action | Error: no computer, wrong PIN, typing failed. The serial port shows the cause. |
| Cyan, blinks | Wi-Fi setup mode. Steady cyan: a phone or a computer is on the network. |
| Blue, blinks, after a hold of 5 s | Pairing of a keypad. Steady blue: type the passkey on the keypad. |
| Purple, blinks | Pairing of a computer. Steady purple: type the passkey on the computer. |
| Dim | No activity for 60 s. A push makes it bright again and does nothing else. |

Texts go to the serial port, as on the Chain Dual Key. `web` and `wireless pair` start the modes without the button.

PIN with the button: tap the button as many times as the digit. For 0, tap it ten times. Then hold the button: after 0.6 s the LED stops to blink and is steady amber. Release: the digit goes in. The LED gives a white tick at each tap and at each digit. When all digits are in, hold the button until the LED is cyan (2 s), then release: the device checks the PIN. Green: unlocked. Red: wrong. A hold of 0.6 s without a tap before it deletes the last digit. The keypad and `unlock` on the serial port also enter the PIN.

The board has no reset button: disconnect the cable and connect it again. The button is also the boot strap pin: hold it while you connect the cable, and the chip starts in the download mode (see "First flash of a new device" in [dev-setup.md](dev-setup.md)).

## PIN

A PIN protects the entries. Without a PIN, a person with the device can type all passwords, and the entries are plain text in flash. With a PIN:

- The entries in flash are encrypted. The key comes from the PIN. See "Security" for the limits.
- The device asks for the PIN after a start, and each time the connection to the computer stops: cable out, computer off, or computer asleep; the Bluetooth computer disconnected.
- Until you enter the PIN, the device types nothing. The command line and the page cannot show or change the entries. The Wi-Fi setup mode and the pairing of a keypad do not start.

The mouse mover, its schedule, and the time sync work also while the device is locked. The keypad keys `*`, `-`, and `+` do not: the keypad gives only the PIN.

### Set the PIN

1. Open the serial command line and type `pin`.
2. Type the PIN two times: 4 to 12 digits. The PIN does not echo.

The entries are encrypted at once. `pin` again changes the PIN. `pin off` removes it: the entries become plain text again. Both work only while the device is unlocked. Make a backup before you set a PIN (see "Backup").

Use 6 or more digits. See "Security" for what a longer PIN gives.

### Enter the PIN

The screen shows a row of items: the digits 0 to 9, `<` (delete the last digit), and `OK`. The stars above the row show how many digits you gave.

- Buttons: a short push of the side button moves the cursor to the right, a long push to the left. A short push of the front button picks the item under the cursor. A long push of the front button is `OK`.
- Keypad: type the digits, then push Enter. Backspace deletes the last digit. While the device is locked, no key goes to the computer.
- Command line: type `unlock`, then the PIN. The PIN then goes through the USB host (see "Security").

The check takes about 1 s (StickC Plus: about 5 s). After 3 wrong PINs in a row, each next try waits two times longer than the last one: 10 s, 20 s, 40 s, and so on, up to 1 h. A restart does not reset the wait. A correct PIN resets the count.

`lock` on the command line locks the device at once, for example before you leave the computer with the cable in it.

### Lost PIN

Nothing can give the entries without the PIN. Type `wipe` on the serial port: it erases the entries and the PIN. Then add the entries again, or import them from a backup on the page.

### Hardware key

The ESP32-S3 can keep a key in eFuse that software cannot read, only the HMAC hardware can use it. With such a key, the PIN key is made from the PIN and this key together: a copy of the flash is useless without the chip. Without it, a copy of the flash is enough to try all PINs on a fast computer (see "Security"). The ESP32 of the StickC Plus has no HMAC unit: this board has no hardware key, and `info` shows "eFuse: no HMAC key".

Burn the key one time. This is permanent, and it uses one of the six key blocks of the chip. In the ESP-IDF environment:

1. Type `dfu` on the serial port. The device restarts into download mode.
2. Make a random key and burn it:
   ```
   head -c 32 /dev/urandom > hmac_key.bin
   espefuse.py --port /dev/ttyACM0 burn_key BLOCK_KEY0 hmac_key.bin HMAC_UP
   ```
   If `BLOCK_KEY0` is in use, take `BLOCK_KEY1` to `BLOCK_KEY5`. `espefuse.py --port /dev/ttyACM0 summary` shows the blocks.
3. Keep `hmac_key.bin` in a safe place, or delete it. With the file, the PIN, and a copy of the flash, a computer can decrypt the entries: this is a way out after a dead device, and a risk if the file gets lost.
4. Push the power button one time. The device starts. `info` shows "eFuse: HMAC key present".
5. Type `pin` and set the PIN again. The entries get a key with the eFuse key. `info` shows "PIN: set, key with the eFuse key".

## Wi-Fi setup mode

Use a phone or a laptop to add and change entries.

1. Hold the two buttons for 2 s, then release them. A bar on the screen shows the progress.
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
| Entries | Push an entry to rename it, change its password, move it, or delete it. To move it, push "Up" or "Down", or type a position and push "Move to". Delete needs two pushes. |
| Add an entry | Type the password two times, or make a random password of 8 to 127 characters. |
| Import | One entry on each line: `label,password`. See below. |
| Backup | Download all entries as an encrypted file. See below. |
| Erase all entries | Type `YES` to confirm. |

Import rules:

- The password is all text after the first comma, also other commas.
- The device removes spaces around the label.
- The device refuses a password that starts or ends with a space, because this is usually a mistake (`label, password`). Use "Add an entry" for such a password.
- If one line is wrong, or the vault does not have space for all lines, the device adds no entries. The message shows the line number.
- The device writes all entries in one step. If the write fails, the vault keeps the old entries.

Like the command line, the page cannot show a password.

### Backup

The backup file contains all labels and passwords. The device encrypts the file with your passphrase. The page and the browser get only the encrypted file.

1. Start the setup mode with the buttons. After a start with the `web` command, the page cannot make a backup (see "Security").
2. Type a passphrase of 12 to 127 printable ASCII characters two times.
3. Push "Download backup". The device needs about 5 s. The browser saves `hw-pass-backup-<date>.txt`.

If the browser does not save the file (for example, the Android sign-in window), the page shows the text. Copy it into a text file.

Keep the passphrase in a safe place. Without it, nobody can read the file, also not you.

To read the file on a computer, use OpenSSL 1.1.1 or later:

```
openssl enc -d -a -aes-256-cbc -pbkdf2 -md sha512 -iter 210000 -in hw-pass-backup-<date>.txt
```

The output has one `label,password` line for each entry: the format of "Import". To put the entries on a device, paste the lines into "Import". Check these lines first:

- A label with a comma. Import uses the first comma as the end of the label.
- A password that starts or ends with a space. Import refuses it. Use "Add an entry".

Delete the decrypted text after use.

File format: the format of `openssl enc -a`. "Salted__", an 8-byte random salt, then AES-256-CBC with PKCS#7 padding, all in base64 lines of 64 characters. PBKDF2-HMAC-SHA512 with 210000 iterations makes the key and the IV from the passphrase and the salt.

## Bluetooth keypad

A Bluetooth LE keyboard can type passwords, for example the numeric keypad A4Tech FBK21C. Each key can type one entry. The device keeps one keypad.

### Pair a keypad

1. Hold the two buttons for 5 s, then release them. After 2 s, the footer shows "Release: Wi-Fi. Hold: keypad". After 5 s, it shows "Release: pair keypad".
2. Put the keypad in pairing mode. FBK21C: hold Enter and `*` (channel BT1), or Enter and `-` (channel BT2), for 3 s. If a PC uses one channel, use the other channel for the device.
3. The screen shows the name of the keypad. If the screen shows a passkey of 6 digits, type the digits on the keypad, then push Enter.
4. The screen shows "Keypad paired".

If the device finds no keypad in 60 s, the screen shows "No keypad found". A button push stops the pairing. If the pairing does not complete, the old keypad stays paired. A new keypad replaces the old keypad.

### Give a key to an entry

1. Select the entry with the side button.
2. Hold a key on the keypad for 2 s, then release it. The footer shows "Hold to bind key" and a bar. After 2 s, it shows "Release: bind key", and a beep sounds.
3. The screen shows "Key 5 types" and the label of the entry.

An entry has one key, and a key has one entry. A new key of the entry replaces its old key. A key that had a different entry moves to the selected entry. To remove the key, hold it again on the same entry: the screen shows "Key 5 removed".

If the keypad disconnects while you hold the key, the key gets no entry.

The list shows the key at the right side of the entry. The `list` command also shows it.

### Type with a key

Push the key and release it in less than 2 s. The device types the password of the entry, without Enter. The selection does not change. The keypad also types when the screen is dark.

A key without an entry types nothing. The screen shows "Key 5: no entry".

The keypad disconnects when it sleeps. A key push wakes it, and it connects again in about 1 s. Some keypads do not send the key that wakes them, and a short push types nothing. Hold the key until "BT" in the header turns green, then release it in less than 2 s: the device reads the key that is down when the keypad connects, and types its entry at the release. Or push the key again.

The keypad does not type in the Wi-Fi setup mode and during a pairing. The other keys also do nothing then.

### Enter and Backspace

Enter and Backspace on the keypad go to the computer, as on a keyboard. For example, type a password with a key, then push Enter. Hold Backspace to delete more characters. A click tells that the key went to the computer. Without a computer, the screen shows "No computer" with the error sound. These two keys type no entry, and you cannot give them to an entry.

If you push a different key while you hold Enter or Backspace, the computer gets the release at once: a held key does not repeat while the device types or shows a message.

### Keys for the device

| Key | Function |
|---|---|
| `-` | Select the previous entry, like a long push of the side button. |
| `+` | Select the next entry, like a short push of the side button. |
| `*` | Turn the mouse mover on or off. The screen shows "Mouse mover on" or "Mouse mover off", and the next change by the schedule, for example "Off at 21:00". |

`-` and `+` move the selection one entry for each push. They also work when the screen is dark: the screen comes on and shows the new selection.

Push `*` and release it in less than 2 s. A longer hold does nothing: the FBK21C uses a hold of Enter and `*` for its pairing mode.

These keys type no entry, and you cannot give them to an entry. If an entry got one of these keys with an older firmware, the key does not type the entry. Give the entry a different key.

### Battery level

If the keypad reports its battery level (Bluetooth Battery service), the header shows it after "BT", for example "BT 85%".

- The device reads the level when the keypad connects. While the keypad is connected, the keypad also sends each change.
- While the keypad sleeps, the screen shows the level of the last connection in gray.
- At 20 % or less, the level is red. Charge or replace the battery of the keypad.
- After a restart, the level shows when the keypad connects again. Push a key to wake the keypad.
- If the keypad does not report the level, the header shows only "BT".

The `info` command also shows the level.

### Remove the keypad

Type `unpair` on the serial port. The entries keep their keys: a new keypad types the same entries with the same keys.

## Bluetooth computer

The device is also a Bluetooth LE keyboard and mouse. A laptop without a USB cable gets the passwords, Enter and Backspace of the keypad, and the moves of the mouse mover. On battery, the device works without a cable. The device keeps one paired computer.

### Pair a computer

1. Hold the two buttons for 8 s, then release them. After 5 s, the footer shows "Release: keypad. Hold: PC". After 8 s, it shows "Release: pair computer". Or type `wireless pair` on the serial port.
2. The screen shows "Waiting". On the computer, open the Bluetooth settings, add a device, and select "hw-pass".
3. The screen shows a passkey of 6 digits (`wireless` on the serial port shows it too). Type the digits on the computer, then push Enter.
4. The screen shows "Computer paired" and the name of the computer.

If no computer pairs in 60 s, the screen shows "No computer". A button push stops the pairing. If the pairing does not complete, the old computer stays paired. A new computer replaces the old computer: also remove "hw-pass" on the old computer.

Windows, macOS, and Linux ask for the passkey. A computer that does not ask for a passkey cannot pair: the device accepts no pairing without it.

### Use the computer

After the pairing, the device advertises while the computer is not connected, and the computer connects by itself when it sees the device: after a start of the device, after a restart of the computer, or when the computer wakes. "BLE" in the header is green when the computer is connected. While the computer is connected, it gets the keys and the mouse moves, also when a USB host is connected. Without the computer, the USB host gets them.

The device locks when the computer disconnects, as it locks when the USB connection stops. Enter the PIN again.

The computer sets the speed of the link. A password of 20 characters takes about 1 s.

### Battery level

The computer shows the battery level of the device (Bluetooth Battery service), and it gets each change. The level comes from the battery voltage, and it moves in steps of 2 %. On USB power, the charger sets the voltage: the level is high while the battery charges. `info` shows the level, the voltage, USB power, and the charge state.

On battery, the screen goes dark after 20 s. The device also asks the computer for a link with a long latency: the radio sleeps between the keys. Some computers connect with a shorter latency.

### Remove the computer

Type `wireless forget` on the serial port. Then remove "hw-pass" in the Bluetooth settings of the computer. If you remove the device on the computer only, the computer cannot connect again: pair it again.

## Mouse mover

The mouse mover keeps the computer awake. The device is also a USB mouse. When the mouse mover is on, the device moves the pointer 1 pixel each 30 s: to the right, and 30 s later back to the left. The computer sees user activity. It does not lock the screen, turn off the display, or go to sleep.

To turn the mouse mover on or off, push `*` on the Bluetooth keypad, or type `mouse` on the serial port. The device keeps the setting after a restart. The default is off.

- The device does not move the pointer while it types. The next move comes 30 s later.
- The device does not know if you use the computer. It also moves the pointer while you work. A move of 1 pixel is too small to see.
- A computer that sleeps does not wake up. The mouse mover only prevents the sleep.
- The computer must read the mouse. Windows, macOS, and Linux desktops do this. A Linux text console without a desktop does not read it: the pointer does not move.

### Schedule

The schedule turns the mouse mover on and off at set times. The default: on at 09:00 and off at 21:00, every day. The schedule is on by default, but it needs the time. See "Time".

| Command | Action |
|---|---|
| `auto` | Turn the schedule on or off. Shows the schedule and the next change. |
| `auto on 09:00 mon-fri` | The mouse mover goes on at 09:00 from Monday to Friday. |
| `auto off 21:00 mon-thu` | The mouse mover goes off at 21:00 from Monday to Thursday. |
| `auto off none` | The mouse mover never goes off by itself. |

Give the time, the days, or both. Days: `all`, `none`, or names of 3 letters: `mon,wed,fri`, `mon-fri`, `fri-mon`.

Rules:

- The schedule changes the mouse mover only at the set times. Between the times, `*` and `mouse` work as always, and the state stays until the next set time. For example, turn the mouse mover on at night: it stays on until the next auto-off.
- A change by the schedule makes the "on" or "off" sound, and the header shows or hides "MM". The device keeps the new state after a restart.
- When you set the time after a power loss, or the first time, the mouse mover gets the state of the last auto-on or auto-off before this time. For example, set the time to 10:00 with the default schedule: the mouse mover goes on, because the auto-on at 09:00 is the last change before 10:00. See "Time".
- A change of the schedule with `auto` has an effect at the next set time. To change the state now, use `*` or `mouse`.
- With an auto-on and an auto-off at the same time, the mouse mover goes off.
- A change can come up to 5 s after the set time.

Example: a work computer that must stay awake over the weekend.

```
auto off 21:00 mon-thu
```

With the default auto-on, the mouse mover is on from 09:00 to 21:00 from Monday to Thursday. On Friday, it goes on at 09:00 and stays on: the next auto-off is on Monday at 21:00. The auto-on on Saturday, Sunday, and Monday changes nothing, because the mouse mover is already on.

`auto`, `mouse`, and `info` show the next change, for example "next: off at 21:00" or "next: on Mon 09:00".

### Time

The schedule needs the local time. The device keeps the time in UTC and shows it in your time zone. Set the zone one time. Then set the time by hand, or let the device get it from an NTP server over Wi-Fi.

#### Time zone

```
tz EET-2EEST,M3.5.0/3,M10.5.0/4
```

The value is a POSIX time zone: the name and the offset of the standard time, then the name of the daylight saving time and the rules for its start and its end. With the rules, the device changes to and from daylight saving time by itself. Without a zone, the device shows UTC. `tz` without a value shows the zone and the time.

| Zone | Value |
|---|---|
| UTC | `UTC0` |
| United Kingdom | `GMT0BST,M3.5.0/1,M10.5.0` |
| Central Europe | `CET-1CEST,M3.5.0,M10.5.0/3` |
| Eastern Europe, Ukraine | `EET-2EEST,M3.5.0/3,M10.5.0/4` |
| US Eastern | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific | `PST8PDT,M3.2.0,M11.1.0` |

On Linux, the last line of a zone file has the value, for example `tail -n 1 /usr/share/zoneinfo/Europe/Kyiv`.

After a change of the zone, the local time moves. The schedule treats this like a correction of the time (see the end of this section).

#### Set the time by hand

```
time 2026-10-31 14:30
```

Give the local time of your zone. `time` without a value shows the time. `info` and the footer of the screen also show it.

The device keeps the time across a restart, but it has no clock chip: a power loss loses the time. This occurs when the computer turns the USB power off and the battery of the device becomes empty. Then the footer shows "--:--", and the schedule waits until the time is set again. When it is set, by you or by the time sync, the mouse mover gets the state of the schedule for this time (see "Schedule").

A program on the computer can set the time at each logon. Then the schedule works without your help. Linux:

```
stty -F /dev/ttyACM0 raw; printf 'time %s\r' "$(date '+%F %R')" > /dev/ttyACM0
```

Windows, in PowerShell, with the COM port of "USB Serial Device":

```
$p = New-Object System.IO.Ports.SerialPort COM5; $p.Open(); $p.Write("time $(Get-Date -Format 'yyyy-MM-dd HH:mm')`r"); $p.Close()
```

#### Time sync over Wi-Fi

The device can get the time from an NTP server: the Wi-Fi router, a computer in your network, or a public server. It joins your Wi-Fi network as a client for some seconds, sends one request, sets the time, and turns Wi-Fi off again.

1. Set the time zone (see above).
2. Type `wifi <network name>`. The device asks for the Wi-Fi password. The password does not echo. The network must use WPA2 or WPA3.
3. Type `ntp`. The device syncs and shows the result, for example `Sync OK: set from 192.168.0.1, correction +0.4 s`.

Then the device syncs by itself: 20 s after each start, and then each 24 hours. After a power loss, it gets the time at the start, and the schedule sets the mouse mover. If a sync fails, the device tries again after 10 minutes, then after 20, 40, and 80 minutes, and then each 4 hours. `info` shows the last result.

| Command | Action |
|---|---|
| `wifi` | Show the network. |
| `wifi <name>` | Set the network. Asks for the password. |
| `wifi off` | Remove the network and the password. Then the device does not sync. |
| `ntp` | Sync now and show the result. |
| `ntp on`, `ntp off` | Turn the automatic sync on or off. The default is on. |
| `ntp server 192.168.0.1` | Use this NTP server: an IP address or a name. |
| `ntp server router` | Use the router: the default gateway of the network. This is the default. |

Not all routers have an NTP server for the devices in the network. On many home routers, the "NTP" setting only sets the clock of the router. If `ntp` shows "no reply from" the router, look for an NTP server option in the router, use a computer in the network with an NTP server (Linux: chrony or ntpd; Windows: the Windows Time service with its `NtpServer` provider enabled), or use a public server: `ntp server pool.ntp.org`. A public server needs internet access from the Wi-Fi network. See "Security".

The device does not sync while the Wi-Fi setup mode is on. A sync that runs stops when the mode starts.

A correction of a known time by less than 12 hours does not lose a change: a correction forward does the changes that it skips, and a correction back does no change two times. A move of 12 hours or more counts as a new time.

## Serial command line

The device is also a USB serial port (CDC-ACM). Use it to add and change entries. Open the port with any terminal program, for example:

- Linux: `picocom /dev/ttyACM0` (exit with `Ctrl+A Ctrl+X`).
- Windows: PuTTY, "Serial", the COM port of "USB Serial Device".

The baud rate has no effect. Push Enter to get the prompt.

| Command | Action |
|---|---|
| `help` | Show the commands. |
| `list` | Show the entries and their keypad keys. |
| `add <label>` | Add an entry. Asks for the password two times. The password does not echo. |
| `gen [len] <label>` | Add an entry with a random password of 8 to 127 characters. The default length is 20. See "Random passwords". |
| `pass <n>` | Change the password of entry n. |
| `name <n> <label>` | Change the label of entry n. |
| `del <n>` | Delete entry n. |
| `move <n> <pos>` | Move entry n to position pos. |
| `wipe` | Erase all entries and the PIN. Works also while the device is locked. |
| `pin [off]` | Set or change the PIN, or remove it. Asks for the PIN two times. See "PIN". |
| `unlock` | Enter the PIN. See "PIN". |
| `lock` | Lock the device now. |
| `layout [key\|off]` | Show or set the layout key: the device pushes it before it types. See "Key layout". |
| `kbtest` | Type all printable ASCII characters. Checks the key layout of the host. |
| `web` | Start the Wi-Fi setup mode and show the network name and the Wi-Fi password. If the mode is on, stop it. |
| `unpair` | Remove the Bluetooth keypad. The entries keep their keys. |
| `wireless [pair\|forget]` | Show the Bluetooth computer, pair one (the screen and `wireless` show the passkey), or remove it. See "Bluetooth computer". |
| `mouse` | Turn the mouse mover on or off. The device keeps the setting. See "Mouse mover". |
| `auto [on\|off HH:MM days]` | Turn the schedule of the mouse mover on or off, or set its times and days. See "Schedule". |
| `time [YYYY-MM-DD HH:MM]` | Show or set the time. See "Time". |
| `tz [zone]` | Show or set the time zone. See "Time". |
| `wifi [<name>\|off]` | Show, set, or remove the Wi-Fi network of the time sync. Asks for the password. See "Time". |
| `ntp [on\|off\|server x]` | Sync the time now, turn the automatic sync on or off, or set the NTP server. See "Time". |
| `sound` | Turn the sounds on or off. The device keeps the setting. See "Sounds". |
| `rotate` | Turn the screen 180 degrees, for a device that lies flat. The turn holds until you turn the device. See "Orientation". |
| `info` | Show the firmware version, the board, and the device state, also the PIN state, the USB host, the battery of the device, the Bluetooth computer, the layout key, the keypad, its battery level, the mouse mover, its schedule, the time, the time zone, the time sync, the sounds, and the tilt. |
| `dfu` | Restart into ROM download mode for flashing. |

Limits:

- 32 entries.
- Label: 1 to 19 characters.
- Password: 1 to 127 characters.
- Only printable ASCII characters (space to `~`).

The command line cannot show a password. Only the front button can type it. While the device is locked, the commands that show or change the entries say so. See "PIN".

## Random passwords

`gen` and the page make passwords with upper and lower case letters, digits, and these symbols: `+ - . : @ _`

These symbols have no special function in bash, zsh, cmd, and PowerShell. You can type such a password in a command line without quotes or escape characters. The first character is always a letter or a digit, because a symbol at the start can be an option (`-x`) or have a special function (`@x` in PowerShell).

Each password has at least one character of each class.

## Typing speed

The device sends one key report each 10 ms: a password of 20 characters takes about 0.5 s. A faster speed can lose keys in a remote desktop (RDP) session or on a slow host. Over Bluetooth, the computer sets the interval of the link, and a password of 20 characters takes about 1 s.

## Key layout

The device sends US key codes. On a host with a different key layout, some characters change (for example, `y` and `z` on a German layout). Use `kbtest` to check the host:

1. Put the focus on the terminal window.
2. Type `kbtest`.
3. The device types the test text, and the terminal sends it back. The device compares the text.

Caps Lock has no effect on the typed text. The device turns Caps Lock off before it types and turns it on again after. If the host does not turn Caps Lock off in 0.5 s, the device types nothing and shows "Caps Lock error". Turn Caps Lock off and try again.

### Layout key

If the host has more than one key layout, the device can push a key before it types. Give the host a shortcut that selects the US or English layout, and give the device the same shortcut with `layout`:

- `layout F16`: a key that a program on the host maps to the English layout, for example AutoHotkey or PowerToys Keyboard Manager on Windows. The keys F13 to F24 are on no usual keyboard: they have no other effect.
- `layout ctrl+shift+1`: a shortcut from the Windows settings. Settings > Time & language > Typing > Advanced keyboard settings > Input language hot keys: give the English layout a key sequence.
- `layout off`: no key. This is the default.

Modifiers: `ctrl`, `shift`, `alt`, `win`. Keys: `F1` to `F24`, `0` to `9`, `a` to `z`, `space`. Only modifiers also work, for example `layout alt+shift`. But Alt+Shift goes to the next layout: it does not select one.

The device pushes the key, waits 0.15 s, then types. `kbtest` also pushes it. The device keeps the setting.

## Security

Read these limits before you store important passwords:

- Flash encryption is off. Without a PIN, a person with the device can read the passwords from the flash with esptool, and can type all passwords.
- With a PIN, the entries in flash are encrypted with AES-256-GCM. The key comes from PBKDF2-HMAC-SHA512 of the PIN, with 50000 iterations: about 1 s on the device. Without the hardware key, a person with a copy of the flash can try PINs on a fast computer: a graphics card tries all 6-digit PINs in less than a minute, and all 8-digit PINs in about an hour. The PIN then stops a casual reader, not an attacker. With the hardware key (see "PIN"), each try needs the chip and takes about 1 s with its own firmware: all 6-digit PINs take 12 days, all 8-digit PINs 3 years. The wait times after wrong PINs apply to this firmware only.
- While the device is unlocked, the key and the entries are in RAM. A lock sets them to zero. The device locks when the USB bus stops: cable out, computer off or asleep. Some hosts stop an idle USB device: then the device asks for the PIN again. The device also locks when the Bluetooth computer disconnects.
- `unlock` and `pin` on the command line send the PIN through the USB host. A program on the host can read it. The buttons and the keypad do not send the PIN to the host.
- `del` and `pass` mark the old data as deleted, but the old bytes stay in flash until NVS reuses the page. With a PIN, the old bytes are encrypted, but after a change of the PIN, old copies with the old key can stay for some time. `wipe` erases the full vault partition.
- `gen` and the random passwords of the page use the hardware random number generator. Its entropy source is the ADC noise when the radio is off, and the radio when Wi-Fi or Bluetooth is on.
- The backup file is as safe as its passphrase. PBKDF2 with 210000 iterations makes each guess slow, but a short or common passphrase is easy to guess. Use 4 or more random words, or 16 or more random characters.

Wi-Fi:

- Wi-Fi is off, except in the Wi-Fi setup mode and during a time sync (some seconds). A build with `CONFIG_HWPASS_WEB=n` and `CONFIG_HWPASS_NTP=n` has no Wi-Fi code.
- Each start of the setup mode makes a new network name and a new random Wi-Fi password of 16 characters (about 79 bits). The network uses WPA2/WPA3 and accepts one device. The transmit power is low (11 dBm).
- The setup mode keeps no Wi-Fi data in flash.
- The page uses HTTP inside the encrypted Wi-Fi link. A person who has the Wi-Fi password while the mode is on can change and erase entries.
- The page can make a backup only if the buttons started the mode. Then only the screen shows the Wi-Fi password. A person who sees the screen while the mode is on can make a backup with their own passphrase, and read all passwords. Such a person can also type all passwords with the front button.
- The `web` command sends the Wi-Fi password to the USB host. A program on the host could join the network and make a backup. Thus after a start with `web`, the page cannot make a backup.
- The page accepts changes only with the session token and the host name `192.168.4.1`. Other web sites in the same browser cannot send changes.
- The firmware sets its copies of a password to zero after use. The Wi-Fi driver and the TCP/IP stack also keep the received bytes in their buffers until other data overwrites them. These buffers are in RAM only. A restart removes them.

Time sync:

- The device keeps the name and the password of your Wi-Fi network in the `nvs` partition. Flash encryption is off: a person with the device can read them with esptool, like the passwords of the entries. `wifi off` removes them.
- The device joins only a network with WPA2 or WPA3. It does not join an open or a WEP network with the same name.
- During a sync, the device is a client in your network for some seconds, with the name `hw-pass`. It opens no port: nothing can connect to it. It sends one UDP request to the NTP server and accepts only a reply from the address and the port of the server. The reply must return the random 64-bit value of the request: a false reply must guess it.
- The device does not check the time of the server. A wrong server gives a wrong time, and the schedule of the mouse mover follows it. With the router as the server, only your network can give the time. A public server answers over the internet, without a signature.
- The sync comes at most one time each 24 hours, or after 10 minutes to 4 hours when it fails. The radio is off between the syncs.

Bluetooth keypad:

- Bluetooth is off until you start a pairing. With a paired keypad or computer, Bluetooth is on: the device waits for the keypad, and advertises for the computer. After `unpair`, `wireless forget`, or a pairing that did not complete, Bluetooth stays on until the next restart, but without a paired device it does not send or receive.
- A person with the paired keypad can type all passwords that have a key, push Enter and Backspace on the computer, and turn on the mouse mover, also when the screen is dark. With a PIN, this needs the PIN first. Keep the keypad as safe as the device.
- Only the buttons can start a pairing. At other times, the device refuses a new pairing. It connects only to the paired keypad, and uses its keys only on a link that the bonding keys encrypt. If the keypad loses its bonding keys, for example after a pairing with a different computer on the same channel, pair it again with the buttons.
- With a passkey, a different device cannot get into the pairing. A keypad without a passkey pairs with "Just Works": a person near you with special equipment can get into such a pairing. Pair such a keypad where no unknown persons are near.
- The bonding keys are in the `nvs` partition. Flash encryption is off: a person with the device can read the bonding keys and make a false keypad. This person can also read the passwords.

Bluetooth computer:

- With a paired computer, the device advertises while the computer is not connected. Outside a pairing, the advertisements are not discoverable: the device does not show in the device lists of other computers, but a scanner sees the name "hw-pass" and that it is a keyboard.
- Only a pairing that you start (the buttons, or `wireless pair`) can pair a computer. The passkey on the screen protects the pairing: a person who does not see the screen cannot complete it. Outside a pairing, the device refuses a new pairing. A connection without the bonding keys of the paired computer ends after 10 s.
- The paired computer gets the passwords that the front button or the keypad types, like a USB host. A person at this computer can read them there. The header shows "BLE" in green while this computer is connected and gets the keys.
- `wireless pair` on the command line starts a pairing from the USB host. A program on the host can start it, but it cannot see the passkey on the screen, and the screen shows the pairing.
- The bonding keys of the computer are in the `nvs` partition, like the keys of the keypad.

Mouse mover:

- When the mouse mover is on, the computer does not lock itself. A person at the computer can use it while you are away. Lock the computer before you go (Windows: Win+L, macOS: Ctrl+Cmd+Q).
- The schedule can turn the mouse mover on while you are away, for example at 09:00 before you arrive. A computer that is not locked then stays open for everyone.
