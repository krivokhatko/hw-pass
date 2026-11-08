#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The serial port: the command line (cli) and the texts of the LED
 * view. A board with USB: the serial port of the USB device (usbdev). A
 * board without USB (StickC Plus): UART0, through the USB bridge of the
 * board. All functions are safe to call from different tasks. */

/* Start the port. Call after usbdev_init. On UART0, log output goes
 * through this module after this call. */
esp_err_t serial_init(void);

/* A terminal holds the port open (DTR is on). UART0 has no DTR: true. */
bool serial_open(void);

/* Read up to size bytes. Return the count, 0 after timeout_ms. */
size_t serial_read(char *buf, size_t size, uint32_t timeout_ms);

/* Write n bytes. LF becomes CR LF. */
void serial_write(const char *s, size_t n);

#ifdef __cplusplus
}
#endif
