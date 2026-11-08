/* Battery of the device. The power chip (M5PM1 on the StickS3, AXP192 on
 * the StickC Plus) or the ADC (Chain Dual Key) measures the voltage of
 * the battery; a discharge curve gives the level in %. The measurement
 * runs in the esp_timer task every BATTERY_PERIOD_S.
 *
 * The level moves in steps of STEP %: a small change of the voltage does
 * not change the level, and the computer does not get a notification for
 * it. 0 % and 100 % are exact. On USB power, the charger sets the voltage:
 * the level is high while the battery charges. */
#include <stdbool.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "battery.h"

#if CONFIG_HWPASS_BATTERY
#include "esp_timer.h"

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

/* ---- The source of the values: the power chip, or the ADC ---- */

#if CONFIG_HWPASS_PM1
#include "pm1.h"

static esp_err_t source_init(void)
{
    return ESP_OK;
}

/* mv 0: no battery. */
static esp_err_t source_read(int *mv, bool *usb, bool *charging)
{
    ESP_RETURN_ON_ERROR(pm1_battery_mv(mv), TAG, "pm1");
    if (pm1_power(usb, charging) != ESP_OK) {
        *usb = false;
        *charging = false;
    }
    return ESP_OK;
}

#elif CONFIG_HWPASS_AXP192
#include "axp192.h"

static esp_err_t source_init(void)
{
    return ESP_OK;
}

/* mv 0: no battery. */
static esp_err_t source_read(int *mv, bool *usb, bool *charging)
{
    ESP_RETURN_ON_ERROR(axp192_battery_mv(mv), TAG, "axp192");
    if (axp192_power(usb, charging) != ESP_OK) {
        *usb = false;
        *charging = false;
    }
    return ESP_OK;
}

#else   /* The ADC of the Chain Dual Key */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "rng.h"

/* Chain Dual Key (schematic V1.5): the battery goes through 510 k and
 * 1 M to G10, the USB voltage through 51 k and 100 k to G2. The pin sees
 * the voltage divided by 1.51. G9 has the state of the TP4057 charger
 * through a resistor network: about 1.65 V while it charges, 2.2 V when
 * the battery is full, 3.3 V without a charge (factory firmware). */
#define CH_BAT          ADC_CHANNEL_9   /* G10 */
#define CH_VBUS         ADC_CHANNEL_1   /* G2 */
#define CH_CHARGE       ADC_CHANNEL_8   /* G9 */
#define DIVIDER_X100    151             /* The voltage is the pin voltage times 1.51. */
#define SAMPLES         8               /* Readings for one value */
#define BAT_MIN_MV      2500            /* Below this: no battery. */
#define USB_MIN_MV      4000            /* USB power above this (factory firmware) */
#define CHARGE_MIN_MV   1400            /* The charger charges between these pin voltages. */
#define CHARGE_MAX_MV   1900

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

static esp_err_t source_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "adc");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, CH_BAT, &chan), TAG, "battery pin");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, CH_VBUS, &chan), TAG, "USB pin");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, CH_CHARGE, &chan), TAG, "charge pin");
    return adc_cali_create_scheme_curve_fitting(&cali, &s_cali);
}

/* The mean of SAMPLES readings, in mV at the pin. */
static esp_err_t read_pin(adc_channel_t ch, int *mv)
{
    int sum = 0, raw, v;

    for (int i = 0; i < SAMPLES; i++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, ch, &raw), TAG, "read");
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, raw, &v), TAG, "cali");
        sum += v;
    }
    *mv = sum / SAMPLES;
    return ESP_OK;
}

/* mv 0: no battery. The ADC noise source of rng must not run now. */
static esp_err_t source_read(int *mv, bool *usb, bool *charging)
{
    int bat = 0, vbus = 0, charge = 0;
    esp_err_t err;

    rng_adc_lock();
    err = read_pin(CH_BAT, &bat);
    if (err == ESP_OK) {
        err = read_pin(CH_VBUS, &vbus);
    }
    if (err == ESP_OK) {
        err = read_pin(CH_CHARGE, &charge);
    }
    rng_adc_unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "adc");
    *mv = bat * DIVIDER_X100 / 100;
    if (*mv < BAT_MIN_MV) {
        *mv = 0;
    }
    *usb = vbus * DIVIDER_X100 / 100 >= USB_MIN_MV;
    *charging = charge >= CHARGE_MIN_MV && charge <= CHARGE_MAX_MV;
    return ESP_OK;
}
#endif  /* The source */

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

    if (source_read(&mv, &usb, &charging) != ESP_OK || mv <= 0) {
        s_mv = 0;
        s_level = -1;
        s_usb = usb;
        s_charging = charging;
        return;
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

    ESP_RETURN_ON_ERROR(source_init(), TAG, "source");
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

#else   /* CONFIG_HWPASS_BATTERY */

/* A board without a battery runs on USB power. There is no level: the
 * Bluetooth battery service reports "unknown". */
static battery_cb_t s_cb;

esp_err_t battery_init(void)
{
    return ESP_OK;
}

int battery_level(void)
{
    return -1;
}

int battery_mv(void)
{
    return 0;
}

bool battery_usb(void)
{
    return true;
}

bool battery_charging(void)
{
    return false;
}

esp_err_t battery_on_change(battery_cb_t cb)
{
    if (s_cb && cb) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    return ESP_OK;
}

#endif  /* CONFIG_HWPASS_BATTERY */
