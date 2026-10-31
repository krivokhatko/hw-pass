/* Two buttons, active low. Debounce by sampling, detect short and long
 * presses, and pushes of both buttons together. */
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "buttons.h"

#define DEBOUNCE_SAMPLES    3

static const gpio_num_t s_pin[BUTTON_COUNT] = { GPIO_NUM_11, GPIO_NUM_12 };

static struct {
    bool down;          /* Debounced state */
    bool long_sent;
    uint8_t changes;    /* Samples that differ from the debounced state */
    uint32_t held_ms;
} s_btn[BUTTON_COUNT];
static uint32_t s_both_ms;
static bool s_pushed;

esp_err_t buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_pin[BUTTON_FRONT]) | (1ULL << s_pin[BUTTON_SIDE]),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&cfg);
}

press_t buttons_poll(button_t b)
{
    bool raw = gpio_get_level(s_pin[b]) == 0;

    if (raw == s_btn[b].down) {
        s_btn[b].changes = 0;
    } else if (++s_btn[b].changes >= DEBOUNCE_SAMPLES) {
        s_btn[b].changes = 0;
        s_btn[b].down = raw;
        if (raw) {
            s_btn[b].held_ms = 0;
            s_btn[b].long_sent = false;
            s_pushed = true;
        } else if (!s_btn[b].long_sent) {
            return PRESS_SHORT;
        }
    }
    /* Both down: no single events for this push, also not at release. */
    if (s_btn[BUTTON_FRONT].down && s_btn[BUTTON_SIDE].down) {
        s_btn[BUTTON_FRONT].long_sent = true;
        s_btn[BUTTON_SIDE].long_sent = true;
    }

    if (s_btn[b].down && !s_btn[b].long_sent) {
        s_btn[b].held_ms += BUTTONS_POLL_MS;
        if (s_btn[b].held_ms >= BUTTONS_LONG_MS) {
            s_btn[b].long_sent = true;
            return PRESS_LONG;
        }
    }
    return PRESS_NONE;
}

uint32_t buttons_both_ms(void)
{
    if (s_btn[BUTTON_FRONT].down && s_btn[BUTTON_SIDE].down) {
        s_both_ms += BUTTONS_POLL_MS;
    } else {
        s_both_ms = 0;
    }
    return s_both_ms;
}

bool buttons_pushed(void)
{
    bool pushed = s_pushed;

    s_pushed = false;
    return pushed;
}

void buttons_consume(void)
{
    for (int b = 0; b < BUTTON_COUNT; b++) {
        s_btn[b].down = gpio_get_level(s_pin[b]) == 0;
        s_btn[b].long_sent = true;
        s_btn[b].changes = 0;
    }
    s_both_ms = 0;
    s_pushed = false;
}
