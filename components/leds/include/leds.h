#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The RGB LEDs under the keys of the Chain Dual Key: one WS2812 for each
 * key, on one data line. LED 0 is under Key2 (select), LED 1 under Key1
 * (type). Call from one task only. */

#define LEDS_COUNT  2

/* Turn on the power of the LEDs and start the data line. The LEDs are
 * off after this. */
esp_err_t leds_init(void);

/* Set the color of LED i, 0..255 for each part. The change shows at
 * leds_show. */
void leds_set(int i, uint8_t r, uint8_t g, uint8_t b);

/* Send the colors to the LEDs. Takes about 70 us; waits for the end. Does
 * nothing when no color changed since the last send. */
void leds_show(void);

#ifdef __cplusplus
}
#endif
