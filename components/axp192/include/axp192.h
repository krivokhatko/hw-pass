#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the AXP192 power chip of the StickC Plus: the LCD power and the
 * charger on, the backlight off, the measurements on. Call before
 * display_init. The power key stays with the chip: a push turns the
 * device on, a hold of 4 s turns it off. */
esp_err_t axp192_init(void);

/* The LCD backlight: the voltage of its regulator. 0 is off, 255 is full.
 * Call from one task only. */
esp_err_t axp192_backlight(uint8_t level);

/* Battery voltage in mV, measured by the chip. 0: no battery. */
esp_err_t axp192_battery_mv(int *mv);

/* Voltage of the USB input in mV. */
esp_err_t axp192_vin_mv(int *mv);

/* Power state. usb: the USB input has power. charging: current goes into
 * the battery. */
esp_err_t axp192_power(bool *usb, bool *charging);

#ifdef __cplusplus
}
#endif
