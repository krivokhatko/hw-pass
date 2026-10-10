#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the M5PM1 power chip. Stop its watchdog, turn off the speaker
 * amplifier, and turn on the LCD power. */
esp_err_t pm1_init(void);

#ifdef __cplusplus
}
#endif
