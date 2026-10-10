# User guide

hw-pass is a USB keyboard that types stored passwords. Select an entry with the buttons. The device types the password into the focused window.

## Buttons

| Button | Short push | Long push (0.6 s) |
|---|---|---|
| Side (KEY2) | Select the next entry | Select the previous entry |
| Front (KEY1, below the screen) | Type the password | Type the password, then push Enter |

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
| `gen [len] <label>` | Add an entry with a random password. The default length is 20. |
| `pass <n>` | Change the password of entry n. |
| `name <n> <label>` | Change the label of entry n. |
| `del <n>` | Delete entry n. |
| `move <n> <pos>` | Move entry n to position pos. |
| `wipe` | Erase all entries. |
| `kbtest` | Type all printable ASCII characters. Checks the key layout of the host. |
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
- `gen` uses the hardware random number generator with the ADC noise source enabled.
