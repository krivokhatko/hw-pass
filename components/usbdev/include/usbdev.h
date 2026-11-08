#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* USB IDs. 0x303A is the Espressif vendor ID, 0x4005 the TinyUSB example
 * PID for CDC + HID. Change the version when the descriptors change:
 * Windows keeps data for each VID, PID, and version. The Bluetooth link
 * (blehid) reports the same IDs. */
#define USBDEV_VID      0x303A
#define USBDEV_PID      0x4005
#define USBDEV_VERSION  0x0101

/* Start the USB device: a boot keyboard, a CDC-ACM serial port, and a
 * boot mouse. After this call, log output goes to the serial port.
 * A chip without USB (ESP32) builds usbdev_none.c: no device, the host
 * is never ready, and the serial functions do nothing. */
esp_err_t usbdev_init(void);

/* The host configured the device and did not suspend it. */
bool usbdev_ready(void);

/* Caps Lock state from the host. */
bool usbdev_caps_lock(void);

/* Send a key report: modifiers and up to 6 keys that are down (HID usage
 * IDs, page 0x07). Waits up to 200 ms for the endpoint. Returns false if
 * the host is not ready or does not take the report. */
bool usbdev_send_keys(uint8_t mod, const uint8_t keys[6]);

/* Send a mouse move. Does not wait: returns false if the endpoint is busy. */
bool usbdev_send_mouse(int8_t dx, int8_t dy);

/* A terminal holds the serial port open (DTR is on). */
bool usbdev_serial_open(void);

/* Read up to size bytes from the serial port. Return the count. */
size_t usbdev_read(char *buf, size_t size, uint32_t timeout_ms);

/* Write to the serial port. LF becomes CR LF. Without DTR, new data
 * replaces old data in the buffer. With DTR, wait a short time for space. */
void usbdev_write(const char *s, size_t n);

/* Restart into ROM download mode on USB Serial/JTAG. Does not return.
 * Without USB: a plain restart (esptool resets the ESP32 through the
 * USB bridge of the board). */
void usbdev_dfu(void);

#ifdef __cplusplus
}
#endif
