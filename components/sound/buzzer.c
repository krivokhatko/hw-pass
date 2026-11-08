/* The passive buzzer of the StickC Plus (G2): square-wave tones from the
 * LEDC PWM. The duty sets the loudness: a plain tone keeps it, a bell
 * note lowers it in steps. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sound_out.h"

#define BUZZER_PIN      2
#define BUZZER_TIMER    LEDC_TIMER_1    /* Timer 0 is the LCD backlight of the StickS3. */
#define BUZZER_CHANNEL  LEDC_CHANNEL_1
#define BUZZER_MODE     LEDC_LOW_SPEED_MODE
#define DUTY_BITS       LEDC_TIMER_10_BIT
#define DUTY_FULL       512     /* 50 %: the loudest square wave */
#define BELL_STEPS      4       /* The level halves at each step. */
#define IDLE_HZ         1000    /* Timer frequency while silent */

static const char *TAG = "buzzer";

static void duty(uint32_t d)
{
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, d);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}

esp_err_t sound_out_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BUZZER_MODE,
        .duty_resolution = DUTY_BITS,
        .timer_num = BUZZER_TIMER,
        .freq_hz = IDLE_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = BUZZER_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "channel");
    ESP_LOGI(TAG, "G%d", BUZZER_PIN);
    return ESP_OK;
}

esp_err_t sound_out_start(void)
{
    return ESP_OK;
}

void sound_out_stop(void)
{
    duty(0);
}

esp_err_t sound_out_tone(uint32_t hz, uint32_t ms, bool bell)
{
    if (hz == 0) {
        duty(0);
        vTaskDelay(pdMS_TO_TICKS(ms));
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, hz), TAG, "%u Hz", (unsigned)hz);
    if (!bell) {
        duty(DUTY_FULL);
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        /* The level falls in steps. The stop of the melody ends the note
         * at the next step. */
        for (int i = 0; i < BELL_STEPS && (i == 0 || sound_melody_on()); i++) {
            duty(DUTY_FULL >> i);
            vTaskDelay(pdMS_TO_TICKS(ms / BELL_STEPS));
        }
    }
    duty(0);
    return ESP_OK;
}
