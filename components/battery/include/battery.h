#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BATTERY_PERIOD_S    20      /* Time between two measurements */

/* Measure the battery, then measure it again every BATTERY_PERIOD_S in
 * the esp_timer task. Call after pm1_init. */
esp_err_t battery_init(void);

/* Battery level in %, 0 to 100, from the voltage and a discharge curve.
 * The level moves in steps of 2 %, and it is exact at 0 % and 100 %.
 * -1: no measurement. */
int battery_level(void);

/* Battery voltage in mV. 0: no measurement. */
int battery_mv(void);

/* The 5 V input (USB) has power. */
bool battery_usb(void);

/* The charger charges the battery. */
bool battery_charging(void);

/* Get a call in the esp_timer task when the level changes. One user. */
typedef void (*battery_cb_t)(int level);
esp_err_t battery_on_change(battery_cb_t cb);

#ifdef __cplusplus
}
#endif
