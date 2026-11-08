/* The internal I2C bus of the board. The pins are those of the board;
 * the chips on the bus set their own clock. */
#include "sdkconfig.h"
#include "esp_log.h"
#include "i2cbus.h"

#if CONFIG_HWPASS_BOARD_STICKCPLUS
#define I2C_SDA     21
#define I2C_SCL     22
#else   /* StickS3 */
#define I2C_SDA     47
#define I2C_SCL     48
#endif

static const char *TAG = "i2cbus";
static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t i2cbus_internal(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (s_bus == NULL) {
        esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bus: %s", esp_err_to_name(err));
            s_bus = NULL;
        }
    }
    return s_bus;
}
