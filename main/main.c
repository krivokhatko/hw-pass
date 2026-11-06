/* Application entry point. Start the modules, then run the user interface. */
#include "esp_log.h"
#include "nvs_flash.h"
#include "ble.h"
#include "blehid.h"
#include "cli.h"
#include "clock.h"
#include "display.h"
#include "hid.h"
#include "imu.h"
#include "keypad.h"
#include "mover.h"
#include "ntp.h"
#include "pm1.h"
#include "rng.h"
#include "sound.h"
#include "sysinfo.h"
#include "ui.h"
#include "usbdev.h"
#include "vault.h"

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
    esp_err_t err;

    sysinfo_log();

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    check(err, "nvs");
    check(pm1_init(), "pm1");
    check(display_init(), "display");
    check(sound_init(), "sound");
    check(imu_init(), "imu");
    check(vault_init(), "vault");
    check(rng_init(), "rng");
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
