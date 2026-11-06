/* Battery of the device. The M5PM1 power chip measures the voltage of the
 * battery; a discharge curve gives the level in %. The measurement runs
 * in the esp_timer task every BATTERY_PERIOD_S.
 *
 * The level moves in steps of STEP %: a small change of the voltage does
 * not change the level, and the computer does not get a notification for
 * it. 0 % and 100 % are exact. On USB power, the charger sets the voltage:
 * the level is high while the battery charges. */
#include <stdbool.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pm1.h"
#include "battery.h"

#define STEP        2       /* Smallest change of the level in % */

/* Voltage of one lithium cell at rest, from 100 % to 0 %. */
static const struct {
    int mv;
    int level;
} s_curve[] = {
    { 4150, 100 }, { 4080, 90 }, { 4000, 80 }, { 3930, 70 }, { 3870, 60 }, { 3820, 50 },
    { 3790, 40 }, { 3760, 30 }, { 3720, 20 }, { 3650, 10 }, { 3400, 0 },
};

static const char *TAG = "battery";
static esp_timer_handle_t s_timer;
static volatile int s_level = -1;
static volatile int s_mv;
static volatile bool s_usb;
static volatile bool s_charging;
static battery_cb_t s_cb;

/* Level in % from the curve, linear between two points. */
static int level_of(int mv)
{
    const size_t n = sizeof(s_curve) / sizeof(s_curve[0]);

    if (mv >= s_curve[0].mv) {
        return 100;
    }
    for (size_t i = 1; i < n; i++) {
        if (mv >= s_curve[i].mv) {
            return s_curve[i].level + (mv - s_curve[i].mv) * (s_curve[i - 1].level - s_curve[i].level) /
                   (s_curve[i - 1].mv - s_curve[i].mv);
        }
    }
    return 0;
}

static void measure(void)
{
    int mv = 0, level, old = s_level;
    bool usb = false, charging = false;

    if (pm1_battery_mv(&mv) != ESP_OK || mv <= 0) {
        s_mv = 0;
        s_level = -1;
        return;
    }
    if (pm1_power(&usb, &charging) != ESP_OK) {
        usb = false;
        charging = false;
    }
    s_mv = mv;
    s_usb = usb;
    s_charging = charging;
    level = level_of(mv);
    if (level != old && (old < 0 || abs(level - old) >= STEP || level == 0 || level == 100)) {
        s_level = level;
        ESP_LOGI(TAG, "%d %%, %d mV%s%s", level, mv, usb ? ", USB power" : "", charging ? ", charging" : "");
        if (s_cb) {
            s_cb(level);
        }
    }
}

static void on_timer(void *arg)
{
    measure();
}

esp_err_t battery_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = on_timer,
        .name = "battery",
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG, "timer");
    measure();
    return esp_timer_start_periodic(s_timer, (uint64_t)BATTERY_PERIOD_S * 1000000);
}

int battery_level(void)
{
    return s_level;
}

int battery_mv(void)
{
    return s_mv;
}

bool battery_usb(void)
{
    return s_usb;
}

bool battery_charging(void)
{
    return s_charging;
}

esp_err_t battery_on_change(battery_cb_t cb)
{
    if (s_cb && cb) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    return ESP_OK;
}
