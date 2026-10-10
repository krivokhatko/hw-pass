/* Two buttons, active low. Debounce by sampling, detect short and long presses. */
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
        } else if (!s_btn[b].long_sent) {
            return PRESS_SHORT;
        }
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
