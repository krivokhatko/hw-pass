/* Two buttons, active low. Debounce by sampling, detect short and long
 * presses, and pushes of both buttons together. A board with one button
 * (CONFIG_HWPASS_BUTTONS == 1) has the front button only: a tap is a
 * short push, and a longer push is a hold, as a hold of both buttons.
 * ui.c takes a release of the hold before UI_BOTH_MS as a push of the
 * side button. */
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "buttons.h"

#define DEBOUNCE_SAMPLES    3

#if CONFIG_HWPASS_BOARD_DUALKEY
/* Chain Dual Key: Key1 (G0, the key far from the lanyard hole) types,
 * Key2 (G17, next to the lanyard hole) selects. Both have a pull-up on
 * the board (schematic). G0 is also the boot strap pin: a hold at power
 * on starts the download mode. */
static const gpio_num_t s_pin[BUTTON_COUNT] = { [BUTTON_FRONT] = GPIO_NUM_0, [BUTTON_SIDE] = GPIO_NUM_17 };
#elif CONFIG_HWPASS_BOARD_STAMPS3A
/* StampS3A: the one button, G0, with a pull-up on the board (schematic
 * v0.3.3). Also the boot strap pin. No side button. */
static const gpio_num_t s_pin[BUTTON_COUNT] = { [BUTTON_FRONT] = GPIO_NUM_0, [BUTTON_SIDE] = GPIO_NUM_NC };
#elif CONFIG_HWPASS_BOARD_STICKCPLUS
/* StickC Plus: button A (G37) below the LCD types, button B (G39) on the
 * side selects. Pull-ups on the board: these pins have no internal ones. */
static const gpio_num_t s_pin[BUTTON_COUNT] = { [BUTTON_FRONT] = GPIO_NUM_37, [BUTTON_SIDE] = GPIO_NUM_39 };
#define BUTTONS_PULLUP  GPIO_PULLUP_DISABLE
#else
/* StickS3: KEY1 below the LCD, KEY2 on the side. */
static const gpio_num_t s_pin[BUTTON_COUNT] = { [BUTTON_FRONT] = GPIO_NUM_11, [BUTTON_SIDE] = GPIO_NUM_12 };
#endif
#ifndef BUTTONS_PULLUP
#define BUTTONS_PULLUP  GPIO_PULLUP_ENABLE
#endif

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
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BUTTONS_PULLUP,
    };

    for (int b = 0; b < BUTTON_COUNT; b++) {
        if (s_pin[b] != GPIO_NUM_NC) {
            cfg.pin_bit_mask |= 1ULL << s_pin[b];
        }
    }
    return gpio_config(&cfg);
}

press_t buttons_poll(button_t b)
{
    bool raw;

    if (s_pin[b] == GPIO_NUM_NC) {
        return PRESS_NONE;
    }
    raw = gpio_get_level(s_pin[b]) == 0;
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
#if CONFIG_HWPASS_BUTTONS == 1
            return PRESS_NONE;      /* The push is a hold: buttons_both_ms gives its time. */
#else
            return PRESS_LONG;
#endif
        }
    }
    return PRESS_NONE;
}

uint32_t buttons_both_ms(void)
{
#if CONFIG_HWPASS_BUTTONS == 1
    /* One button: a push that lasts BUTTONS_LONG_MS or longer is a hold.
     * The time counts from the start of the push. */
    if (s_btn[BUTTON_FRONT].down) {
        s_both_ms += BUTTONS_POLL_MS;
        return s_both_ms >= BUTTONS_LONG_MS ? s_both_ms : 0;
    }
    s_both_ms = 0;
    return 0;
#else
    if (s_btn[BUTTON_FRONT].down && s_btn[BUTTON_SIDE].down) {
        s_both_ms += BUTTONS_POLL_MS;
    } else {
        s_both_ms = 0;
    }
    return s_both_ms;
#endif
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
        s_btn[b].down = s_pin[b] != GPIO_NUM_NC && gpio_get_level(s_pin[b]) == 0;
        s_btn[b].long_sent = true;
        s_btn[b].changes = 0;
    }
    s_both_ms = 0;
    s_pushed = false;
}
