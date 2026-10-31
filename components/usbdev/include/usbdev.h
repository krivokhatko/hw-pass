#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Period of the mouse mover. The usual minimum of the screen lock and
 * sleep times is 1 min. */
#define USBDEV_MOUSE_PERIOD_S   30

/* Start the USB device: a boot keyboard, a CDC-ACM serial port, and a
 * boot mouse. After this call, log output goes to the serial port. */
esp_err_t usbdev_init(void);

/* The host configured the device and did not suspend it. */
bool usbdev_ready(void);

/* Caps Lock state from the host. */
bool usbdev_caps_lock(void);

/* Type printable ASCII text with the US key layout. Caps Lock does not
 * change the result. If enter is true, push Enter at the end. Before the
 * text, the device pushes the layout key, if one is set.
 * Returns ESP_ERR_INVALID_RESPONSE and types nothing if the host does
 * not turn Caps Lock off. */
esp_err_t usbdev_type(const char *s, bool enter);

/* Layout key: a key that the device pushes before it types, for a host
 * with more than one key layout. The host has a shortcut that selects
 * the US or English layout, for example F16 or Ctrl+Shift+1. Text: "off",
 * or modifiers and one key with "+" between: ctrl, shift, alt, win, and
 * F1 to F24, 0 to 9, a to z, or space. Only modifiers also work. The
 * setting is kept in NVS. ESP_ERR_INVALID_ARG: unknown text. */
#define USBDEV_LAYOUT_TEXT  24
esp_err_t usbdev_set_layout(const char *text);
void usbdev_layout_text(char *buf, size_t size);

/* Push (down) or release a key: HID usage ID, page 0x07. The key stays
 * down until its release, and the host repeats it. Key 0 releases all
 * keys. Typing also releases them. After an error, the next call sends
 * the keys again. */
esp_err_t usbdev_key(uint8_t key, bool down);

/* Mouse mover. While it is on, the mouse pointer moves 1 pixel each
 * USBDEV_MOUSE_PERIOD_S: to the right, then back to the left. The host
 * sees user activity: it does not lock the screen or sleep. No move
 * while the device types or the host is not ready. Default: off. */
void usbdev_mouse_mover(bool on);
bool usbdev_mouse_mover_on(void);

/* A terminal holds the serial port open (DTR is on). */
bool usbdev_serial_open(void);

/* Read up to size bytes from the serial port. Return the count. */
size_t usbdev_read(char *buf, size_t size, uint32_t timeout_ms);

/* Write to the serial port. LF becomes CR LF. Without DTR, new data
 * replaces old data in the buffer. With DTR, wait a short time for space. */
void usbdev_write(const char *s, size_t n);

/* Restart into ROM download mode on USB Serial/JTAG. Does not return. */
void usbdev_dfu(void);

#ifdef __cplusplus
}
#endif
