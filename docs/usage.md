# User guide

hw-pass is a USB keyboard that types stored passwords. Select an entry with the buttons. The device types the password into the focused window.

## Buttons

| Button | Short push | Long push (0.6 s) |
|---|---|---|
| Side (KEY2) | Select the next entry | Select the previous entry |
| Front (KEY1, below the screen) | Type the password | Type the password, then push Enter |
| Both together | - | Hold 2 s, then release: start the Wi-Fi setup mode. Hold 5 s, then release: pair a Bluetooth keypad. |

Push the two buttons at the same time. If you hold the front button alone for 0.6 s, the device types the password. The footer shows what occurs when you release the two buttons.

The screen goes dark after 60 s without activity. The first push turns it on again and does nothing else.

The power button on the other side belongs to the power chip:

- One push: power on or reset.
- Two pushes: power off.
- Long push: ROM download mode (for flashing).

## Screen

```
hw-pass  3/12  Mouse BT  USB  <- position, mouse mover, keypad state, USB state (green: ready)
  GitHub                 1    <- the keypad key of the entry
  Mail                        <- the selected entry has a color bar
  Bank                   2
------------------------------
Side: next  Front: type
```

"BT" shows only when a keypad is paired. Green: the keypad is connected. Gray: the device waits for the keypad.

"Mouse" shows only when the mouse mover is on. Green: the USB host is ready. Gray: the pointer does not move.

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
2. Hold a key on the keypad for 2 s, then release it. The footer shows "Hold to bind key" and a bar. After 2 s, it shows "Release: bind key".
3. The screen shows "Key 5 types" and the label of the entry.

An entry has one key, and a key has one entry. A new key of the entry replaces its old key. A key that had a different entry moves to the selected entry. To remove the key, hold it again on the same entry: the screen shows "Key 5 removed".

If the keypad disconnects while you hold the key, the key gets no entry.

The list shows the key at the right side of the entry. The `list` command also shows it.

### Type with a key

Push the key and release it in less than 2 s. The device types the password of the entry, without Enter. The selection does not change. The keypad also types when the screen is dark.

A key without an entry types nothing. The screen shows "Key 5: no entry".

The keypad disconnects when it sleeps. A key push wakes it, and it connects again in about 1 s. Some keypads do not send the key that wakes them. Then push the key again.

The keypad does not type in the Wi-Fi setup mode and during a pairing. Enter and Backspace also do not go to the computer then.

### Enter and Backspace

Enter and Backspace on the keypad go to the computer, as on a keyboard. For example, type a password with a key, then push Enter. Hold Backspace to delete more characters. These two keys type no entry, and you cannot give them to an entry.

If you push a different key while you hold Enter or Backspace, the computer gets the release at once: a held key does not repeat while the device types or shows a message.

### Remove the keypad

Type `unpair` on the serial port. The entries keep their keys: a new keypad types the same entries with the same keys.

## Mouse mover

The mouse mover keeps the computer awake. The device is also a USB mouse. When the mouse mover is on, the device moves the pointer 1 pixel each 30 s: to the right, and 30 s later back to the left. The computer sees user activity. It does not lock the screen, turn off the display, or go to sleep.

Type `mouse` on the serial port to turn the mouse mover on or off. The device keeps the setting after a restart. The default is off.

- The device does not move the pointer while it types. The next move comes 30 s later.
- The device does not know if you use the computer. It also moves the pointer while you work. A move of 1 pixel is too small to see.
- A computer that sleeps does not wake up. The mouse mover only prevents the sleep.
- The computer must read the mouse. Windows, macOS, and Linux desktops do this. A Linux text console without a desktop does not read it: the pointer does not move.

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
| `wipe` | Erase all entries. |
| `kbtest` | Type all printable ASCII characters. Checks the key layout of the host. |
| `web` | Start the Wi-Fi setup mode and show the network name and the Wi-Fi password. If the mode is on, stop it. |
| `unpair` | Remove the Bluetooth keypad. The entries keep their keys. |
| `mouse` | Turn the mouse mover on or off. The device keeps the setting. See "Mouse mover". |
| `rotate` | Turn the screen 180 degrees. The device keeps the setting. |
| `info` | Show the firmware version and the device state, also the keypad and the mouse mover. |
| `dfu` | Restart into ROM download mode for flashing. |

Limits:

- 32 entries.
- Label: 1 to 19 characters.
- Password: 1 to 127 characters.
- Only printable ASCII characters (space to `~`).

The command line cannot show a password. Only the front button can type it.

## Random passwords

`gen` and the page make passwords with upper and lower case letters, digits, and these symbols: `+ - . : @ _`

These symbols have no special function in bash, zsh, cmd, and PowerShell. You can type such a password in a command line without quotes or escape characters. The first character is always a letter or a digit, because a symbol at the start can be an option (`-x`) or have a special function (`@x` in PowerShell).

Each password has at least one character of each class.

## Typing speed

The device sends one key report each 10 ms: a password of 20 characters takes about 0.5 s. A faster speed can lose keys in a remote desktop (RDP) session or on a slow host.

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
- `gen` and the random passwords of the page use the hardware random number generator. Its entropy source is the ADC noise when the radio is off, and the radio when Wi-Fi or Bluetooth is on.
- The backup file is as safe as its passphrase. PBKDF2 with 210000 iterations makes each guess slow, but a short or common passphrase is easy to guess. Use 4 or more random words, or 16 or more random characters.

Wi-Fi:

- Wi-Fi is off, except in the Wi-Fi setup mode. A build with `CONFIG_HWPASS_WEB=n` has no Wi-Fi code.
- Each start makes a new network name and a new random Wi-Fi password of 16 characters (about 79 bits). The network uses WPA2/WPA3 and accepts one device. The transmit power is low (11 dBm).
- The device keeps no Wi-Fi data in flash.
- The page uses HTTP inside the encrypted Wi-Fi link. A person who has the Wi-Fi password while the mode is on can change and erase entries.
- The page can make a backup only if the buttons started the mode. Then only the screen shows the Wi-Fi password. A person who sees the screen while the mode is on can make a backup with their own passphrase, and read all passwords. Such a person can also type all passwords with the front button.
- The `web` command sends the Wi-Fi password to the USB host. A program on the host could join the network and make a backup. Thus after a start with `web`, the page cannot make a backup.
- The page accepts changes only with the session token and the host name `192.168.4.1`. Other web sites in the same browser cannot send changes.
- The firmware sets its copies of a password to zero after use. The Wi-Fi driver and the TCP/IP stack also keep the received bytes in their buffers until other data overwrites them. These buffers are in RAM only. A restart removes them.

Bluetooth keypad:

- Bluetooth is off until you start a pairing. With a paired keypad, Bluetooth is on and the device waits for the keypad. After `unpair` or a pairing that did not complete, Bluetooth stays on until the next restart, but it does not send or receive.
- A person with the paired keypad can type all passwords that have a key, and push Enter and Backspace on the computer, also when the screen is dark. Keep the keypad as safe as the device.
- Only the buttons can start a pairing. At other times, the device refuses a new pairing. It connects only to the paired keypad, and uses its keys only on a link that the bonding keys encrypt. If the keypad loses its bonding keys, for example after a pairing with a different computer on the same channel, pair it again with the buttons.
- With a passkey, a different device cannot get into the pairing. A keypad without a passkey pairs with "Just Works": a person near you with special equipment can get into such a pairing. Pair such a keypad where no unknown persons are near.
- The bonding keys are in the `nvs` partition. Flash encryption is off: a person with the device can read the bonding keys and make a false keypad. This person can also read the passwords.

Mouse mover:

- When the mouse mover is on, the computer does not lock itself. A person at the computer can use it while you are away. Lock the computer before you go (Windows: Win+L, macOS: Ctrl+Cmd+Q).
