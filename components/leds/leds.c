/* WS2812 LEDs on one RMT channel. Each bit is one RMT symbol: a high
 * time, then a low time. 0: 0.3 us high, 0.9 us low. 1: 0.9 us high,
 * 0.3 us low (WS2812B data sheet: T0H 0.22-0.38, T1H 0.58-1.0, with a
 * 1.25 us period). A low time of 300 us at the end latches the colors
 * (the data sheet asks for 50 us; newer parts need 280 us). The LEDs take
 * the bytes green, red, blue, high bit first. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "leds.h"

#if CONFIG_HWPASS_LEDS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#if CONFIG_HWPASS_BOARD_STAMPS3A
/* StampS3A: one WS2812B-2020 on G21. G38 is the enable of the load switch
 * that powers the LED and the backlight of the optional LCD: push-pull,
 * high is on (schematic v0.3.3). */
#define LEDS_PIN            21
#define LEDS_POWER_PIN      38
#define LEDS_POWER_MODE     GPIO_MODE_OUTPUT
#define LEDS_POWER_ON       1
#else
/* Chain Dual Key: two WS2812B-2020 on G21, LED 0 under Key2, LED 1 under
 * Key1. G40 is the gate of the power switch of the LEDs: open drain, low
 * is on (schematic V1.5 and the factory firmware). */
#define LEDS_PIN            21
#define LEDS_POWER_PIN      40
#define LEDS_POWER_MODE     GPIO_MODE_OUTPUT_OD
#define LEDS_POWER_ON       0
#endif
#define POWER_UP_MS     5           /* The switch is on and the LEDs are ready after this time. */
#define RMT_HZ          10000000    /* 0.1 us for each tick */
#define T0H             3
#define T0L             9
#define T1H             9
#define T1L             3
#define RESET_TICKS     3000        /* 300 us */
#define BITS            (LEDS_COUNT * 24)
#define WAIT_MS         10

static const char *TAG = "leds";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static uint8_t s_grb[LEDS_COUNT][3];
static rmt_symbol_word_t s_symbols[BITS + 1];
static bool s_dirty;

esp_err_t leds_init(void)
{
    rmt_tx_channel_config_t chan = {
        .gpio_num = LEDS_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    rmt_copy_encoder_config_t copy = {};
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << LEDS_POWER_PIN,
        .mode = LEDS_POWER_MODE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "power pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(LEDS_POWER_PIN, LEDS_POWER_ON), TAG, "power on");
    vTaskDelay(pdMS_TO_TICKS(POWER_UP_MS));
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan, &s_chan), TAG, "channel");
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy, &s_encoder), TAG, "encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "enable");
    memset(s_grb, 0, sizeof(s_grb));
    s_dirty = true;
    leds_show();
    return ESP_OK;
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= LEDS_COUNT) {
        return;
    }
    if (s_grb[i][0] != g || s_grb[i][1] != r || s_grb[i][2] != b) {
        s_grb[i][0] = g;
        s_grb[i][1] = r;
        s_grb[i][2] = b;
        s_dirty = true;
    }
}

void leds_show(void)
{
    rmt_transmit_config_t tx = { .loop_count = 0 };
    const uint8_t *bytes = &s_grb[0][0];
    int n = 0;

    if (!s_dirty || s_chan == NULL) {
        return;
    }
    for (int i = 0; i < LEDS_COUNT * 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (bytes[i] >> bit) & 1;

            s_symbols[n].level0 = 1;
            s_symbols[n].duration0 = one ? T1H : T0H;
            s_symbols[n].level1 = 0;
            s_symbols[n].duration1 = one ? T1L : T0L;
            n++;
        }
    }
    s_symbols[n].level0 = 0;
    s_symbols[n].duration0 = RESET_TICKS;
    s_symbols[n].level1 = 0;
    s_symbols[n].duration1 = RESET_TICKS;
    if (rmt_transmit(s_chan, s_encoder, s_symbols, sizeof(s_symbols), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, WAIT_MS);
    }
    s_dirty = false;
}

#else   /* CONFIG_HWPASS_LEDS */

/* A board without key LEDs. */
esp_err_t leds_init(void)
{
    return ESP_OK;
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
}

void leds_show(void)
{
}

#endif  /* CONFIG_HWPASS_LEDS */
