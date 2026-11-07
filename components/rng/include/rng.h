#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* Fill buf with len random bytes. */
void rng_bytes(void *buf, size_t len);

/* Fill buf with a random password of len characters (4 or more), then a
 * NUL. It has upper and lower case letters, digits, and symbols that need
 * no escape in a shell: + - . : @ _
 * The first character is a letter or a digit. */
void rng_password(char *buf, int len);

typedef enum {
    RNG_RADIO_WIFI = 1 << 0,
    RNG_RADIO_BT = 1 << 1,
} rng_radio_t;

/* The ADC noise source and the radio must not run at the same time.
 * Hold the lock while a radio starts or stops, then give its new state. */
void rng_radio_lock(void);
void rng_radio_unlock(rng_radio_t radio, bool on);

/* The ADC noise source and a measurement with the ADC must not run at
 * the same time. Hold the lock during the measurement. */
void rng_adc_lock(void);
void rng_adc_unlock(void);

#ifdef __cplusplus
}
#endif
