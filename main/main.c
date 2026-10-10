/* Application entry point. Start the modules, then run the heartbeat. */
#include <inttypes.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sysinfo.h"

static const char *TAG = "main";

void app_main(void)
{
    sysinfo_log();

    for (uint32_t n = 0;; n++) {
        ESP_LOGI(TAG, "alive %" PRIu32, n);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
