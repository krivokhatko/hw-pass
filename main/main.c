/* Application entry point. Start the modules, then run the user interface. */
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "battery.h"
#include "ble.h"
#include "blehid.h"
#include "cli.h"
#include "clock.h"
#include "hid.h"
#include "keypad.h"
#include "mover.h"
#include "ntp.h"
#include "rng.h"
#include "sound.h"
#include "sysinfo.h"
#include "ui.h"
#include "usbdev.h"
#include "vault.h"
/* The parts that not every board has. The other modules build for each
 * board; without the part, their functions do nothing. */
#if CONFIG_HWPASS_PM1
#include "pm1.h"
#endif
#if CONFIG_HWPASS_LCD
#include "display.h"
#endif
#if CONFIG_HWPASS_IMU
#include "imu.h"
#endif

static const char *TAG = "main";

/* Log an error and continue. The USB serial port must stay available,
 * because it is the only way to restart into download mode. */
static void check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    }
}

void app_main(void)
{
    /* Battery: 80 MHz while all tasks wait. The APB clock stays at 80 MHz
     * at both frequencies: the peripherals see no change. */
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t err;

    sysinfo_log();
    check(esp_pm_configure(&pm), "pm");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    check(err, "nvs");
    check(rng_init(), "rng");           /* Before battery_init: a battery read by ADC takes the RNG lock. */
#if CONFIG_HWPASS_PM1
    check(pm1_init(), "pm1");
#endif
    check(battery_init(), "battery");
#if CONFIG_HWPASS_LCD
    check(display_init(), "display");
#endif
    check(sound_init(), "sound");
#if CONFIG_HWPASS_IMU
    check(imu_init(), "imu");
#endif
    check(vault_init(), "vault");
    check(clock_init(), "clock");
    check(usbdev_init(), "usb");
    check(ble_init(), "ble");
    check(blehid_init(), "blehid");     /* Before keypad_init: its services must be in the host before the start. */
    check(hid_init(), "hid");
    check(mover_init(), "mover");
    check(keypad_init(), "keypad");
    check(ntp_init(), "ntp");
    check(cli_start(), "cli");
    ui_run();
}
