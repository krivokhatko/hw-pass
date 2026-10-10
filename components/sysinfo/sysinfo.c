#include <inttypes.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "sysinfo.h"

static const char *TAG = "sysinfo";

void sysinfo_log(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "%s rev v%d.%d, %d cores, %" PRIu32 " MB flash",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             chip.cores, flash_size >> 20);
}
