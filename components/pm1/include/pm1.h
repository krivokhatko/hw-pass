#pragma once

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the M5PM1 power chip. Stop its watchdog, turn off the speaker
 * amplifier, and turn on the LCD power. */
esp_err_t pm1_init(void);

/* The internal I2C bus. The ES8311 audio codec is also on this bus.
 * NULL before pm1_init. */
i2c_master_bus_handle_t pm1_bus(void);

/* Turn the speaker amplifier (AW8737) on or off. Call from one task
 * only: the output register of the PM1 also has the LCD power. */
esp_err_t pm1_speaker(bool on);

/* Battery voltage in mV, measured by the chip. 0: no battery. */
esp_err_t pm1_battery_mv(int *mv);

/* Voltage of the 5 V input (USB) in mV. */
esp_err_t pm1_vin_mv(int *mv);

/* Power state. usb: the 5 V input has power. charging: the charger
 * charges the battery (its status pin is on PM1 G0). */
esp_err_t pm1_power(bool *usb, bool *charging);

#ifdef __cplusplus
}
#endif
