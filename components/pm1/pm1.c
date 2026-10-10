/* M5PM1 power chip on the internal I2C bus. Register map from M5Unified. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pm1.h"

#define PM1_SDA         47
#define PM1_SCL         48
#define PM1_ADDR        0x6E
#define PM1_HZ          100000
#define PM1_TIMEOUT_MS  50

#define REG_DEVICE_ID   0x00
#define REG_I2C_CFG     0x09    /* 0: no I2C idle sleep */
#define REG_WDT_CNT     0x0A    /* 0: watchdog off */
#define REG_GPIO_MODE   0x10    /* 1: output */
#define REG_GPIO_OUT    0x11
#define REG_GPIO_DRV    0x13    /* 0: push-pull */
#define REG_GPIO_FUNC0  0x16    /* 0: GPIO function */

#define PIN_LCD_POWER   (1 << 2)    /* PM1 G2: LCD power (L3B enable) */
#define PIN_SPEAKER     (1 << 3)    /* PM1 G3: speaker amplifier enable */

static const char *TAG = "pm1";
static i2c_master_dev_handle_t s_dev;

static esp_err_t pm1_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, PM1_TIMEOUT_MS);
}

static esp_err_t pm1_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), PM1_TIMEOUT_MS);
}

static esp_err_t pm1_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t val;
    ESP_RETURN_ON_ERROR(pm1_read(reg, &val), TAG, "read 0x%02x", reg);
    return pm1_write(reg, (val & ~mask) | (bits & mask));
}

/* Make a PM1 pin a push-pull output with the given level. */
static esp_err_t pm1_output(uint8_t pin, bool level)
{
    ESP_RETURN_ON_ERROR(pm1_update(REG_GPIO_FUNC0, pin, 0), TAG, "func");
    ESP_RETURN_ON_ERROR(pm1_update(REG_GPIO_MODE, pin, pin), TAG, "mode");
    ESP_RETURN_ON_ERROR(pm1_update(REG_GPIO_DRV, pin, 0), TAG, "drive");
    return pm1_update(REG_GPIO_OUT, pin, level ? pin : 0);
}

esp_err_t pm1_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PM1_SDA,
        .scl_io_num = PM1_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PM1_ADDR,
        .scl_speed_hz = PM1_HZ,
    };
    i2c_master_bus_handle_t bus;
    uint8_t id = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "bus");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "device");

    /* The chip can sleep on the bus. The first access wakes it. */
    for (int i = 0; i < 5; i++) {
        err = pm1_read(REG_DEVICE_ID, &id);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_RETURN_ON_ERROR(err, TAG, "no answer at 0x%02x", PM1_ADDR);

    ESP_RETURN_ON_ERROR(pm1_write(REG_I2C_CFG, 0x00), TAG, "i2c cfg");
    ESP_RETURN_ON_ERROR(pm1_write(REG_WDT_CNT, 0x00), TAG, "wdt");
    ESP_RETURN_ON_ERROR(pm1_output(PIN_SPEAKER, false), TAG, "speaker");
    ESP_RETURN_ON_ERROR(pm1_output(PIN_LCD_POWER, true), TAG, "lcd power");
    /* Let the LCD supply become stable before the LCD reset. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "id 0x%02x, LCD power on", id);
    return ESP_OK;
}
