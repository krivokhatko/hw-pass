#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keyboard and mouse output. The device is a keyboard and a mouse on two
 * links: USB (usbdev) and Bluetooth LE (blehid). This module types text,
 * holds keys, and moves the pointer, and selects the link: the Bluetooth
 * computer when it is connected, else the USB host. All functions are
 * safe to call from different tasks. */

/* Period of the mouse mover. The usual minimum of the screen lock and
 * sleep times is 1 min. */
#define HID_MOUSE_PERIOD_S  30

typedef enum {
    HID_LINK_NONE,      /* No computer is ready. */
    HID_LINK_USB,
    HID_LINK_BLE,
} hid_link_t;

/* Load the layout key and start the mouse mover timer. Call after
 * usbdev_init and blehid_init. */
esp_err_t hid_init(void);

/* The link that gets the keys now. */
hid_link_t hid_link(void);

/* A computer is ready for keys. */
bool hid_ready(void);

/* Caps Lock state from the computer. */
bool hid_caps_lock(void);

/* Type printable ASCII text with the US key layout. Caps Lock does not
 * change the result. If enter is true, push Enter at the end. Before the
 * text, the device pushes the layout key, if one is set. The text goes to
 * the link that is ready at the start.
 * Returns ESP_ERR_INVALID_STATE if no computer is ready, and
 * ESP_ERR_INVALID_RESPONSE and types nothing if the computer does not
 * turn Caps Lock off. */
esp_err_t hid_type(const char *s, bool enter);

/* Layout key: a key that the device pushes before it types, for a
 * computer with more than one key layout. The computer has a shortcut
 * that selects the US or English layout, for example F16 or Ctrl+Shift+1.
 * Text: "off", or modifiers and one key with "+" between: ctrl, shift,
 * alt, win, and F1 to F24, 0 to 9, a to z, or space. Only modifiers also
 * work. The setting is kept in NVS. ESP_ERR_INVALID_ARG: unknown text. */
#define HID_LAYOUT_TEXT     24
esp_err_t hid_set_layout(const char *text);
void hid_layout_text(char *buf, size_t size);

/* Push (down) or release a key: HID usage ID, page 0x07. The key stays
 * down until its release, and the computer repeats it. Key 0 releases all
 * keys. Typing also releases them. After an error, the next call sends
 * the keys again. */
esp_err_t hid_key(uint8_t key, bool down);

/* Mouse mover. While it is on, the mouse pointer moves 1 pixel each
 * HID_MOUSE_PERIOD_S: to the right, then back to the left. The computer
 * sees user activity: it does not lock the screen or sleep. No move while
 * the device types or no computer is ready. Default: off. */
void hid_mouse_mover(bool on);
bool hid_mouse_mover_on(void);

#ifdef __cplusplus
}
#endif
