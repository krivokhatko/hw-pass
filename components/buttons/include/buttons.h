#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sample interval in ms. */
#define BUTTONS_POLL_MS     10
/* Time in ms that makes a press a long press. */
#define BUTTONS_LONG_MS     600

typedef enum {
    BUTTON_FRONT,   /* KEY1, G11, below the LCD */
    BUTTON_SIDE,    /* KEY2, G12, on the side */
    BUTTON_COUNT,
} button_t;

typedef enum {
    PRESS_NONE,
    PRESS_SHORT,    /* Reported at release. */
    PRESS_LONG,     /* Reported after BUTTONS_LONG_MS while held. */
} press_t;

esp_err_t buttons_init(void);

/* Sample a button and return its event. Call every BUTTONS_POLL_MS. */
press_t buttons_poll(button_t b);

#ifdef __cplusplus
}
#endif
