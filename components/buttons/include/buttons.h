#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sample interval in ms. */
#define BUTTONS_POLL_MS     10
/* Time in ms that makes a press a long press. */
#define BUTTONS_LONG_MS     600

typedef enum {
    BUTTON_FRONT,   /* Types. StickS3: KEY1, G11, below the LCD. StickC Plus: A, G37. Dual Key: Key1, G0. StampS3A: the button, G0. */
    BUTTON_SIDE,    /* Selects. StickS3: KEY2, G12, on the side. StickC Plus: B, G39. Dual Key: Key2, G17. StampS3A: none. */
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

/* Return the time in ms that both buttons are down together, else 0.
 * A push of both buttons gives no single events, also not at release.
 * Call one time in each poll cycle, after buttons_poll for both buttons.
 * A board with one button (CONFIG_HWPASS_BUTTONS == 1): the time of a
 * push that lasts BUTTONS_LONG_MS or longer, from its start. Such a push
 * gives no short and no long event. */
uint32_t buttons_both_ms(void);

/* Return true if a button went down since the last call. Its action comes
 * later: at the release or after a hold time. */
bool buttons_pushed(void);

/* Ignore the pushes that are in progress: a button that is down gives no
 * event, also not at release. Call after a long time without buttons_poll. */
void buttons_consume(void);

#ifdef __cplusplus
}
#endif
