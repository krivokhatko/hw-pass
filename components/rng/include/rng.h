#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNG_PASSWORD_MIN        8
#define RNG_PASSWORD_DEFAULT    20

/* Call one time before the other functions. */
esp_err_t rng_init(void);

/* Fill buf with len random characters from set, then a NUL. All
 * characters have the same probability. */
void rng_text(char *buf, int len, const char *set);

/* Fill buf with a random password of len characters (4 or more), then a
 * NUL. It has upper and lower case letters, digits, and symbols. */
void rng_password(char *buf, int len);

/* The ADC noise source and the radio must not run at the same time.
 * Hold the lock while the radio starts or stops, then give the new
 * radio state. */
void rng_radio_lock(void);
void rng_radio_unlock(bool radio_on);

#ifdef __cplusplus
}
#endif
