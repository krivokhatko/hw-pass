/* M5PM1 power chip on the internal I2C bus. Register map from M5Unified. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2cbus.h"
#include "pm1.h"

#define PM1_ADDR        0x6E
#define PM1_HZ          100000
#define PM1_TIMEOUT_MS  50
#define PM1_TRIES       3       /* I2C attempts for one access: the chip can miss one. */
#define PM1_RETRY_MS    1

#define REG_DEVICE_ID   0x00
#define REG_PWR_SRC     0x04    /* Power sources: bit 0 5 V input, bit 1 5 V in/out, bit 2 battery node */
#define REG_I2C_CFG     0x09    /* 0: no I2C idle sleep */
#define REG_WDT_CNT     0x0A    /* 0: watchdog off */
#define REG_GPIO_MODE   0x10    /* 1: output */
#define REG_GPIO_OUT    0x11
#define REG_GPIO_IN     0x12
#define REG_GPIO_DRV    0x13    /* 0: push-pull */
#define REG_GPIO_FUNC0  0x16    /* 0: GPIO function */
#define REG_VBAT_L      0x22    /* Battery voltage in mV, 2 bytes, low byte first */
#define REG_VIN_L       0x24    /* 5 V input voltage in mV, 2 bytes, low byte first */

#define PWR_SRC_VIN     (1 << 0)    /* The 5 V input has power. */

#define PIN_CHG_STAT    (1 << 0)    /* PM1 G0: status of the charger, low = charging */
#define PIN_LCD_POWER   (1 << 2)    /* PM1 G2: LCD power (L3B enable) */
#define PIN_SPEAKER     (1 << 3)    /* PM1 G3: speaker amplifier enable */

static const char *TAG = "pm1";
static i2c_master_dev_handle_t s_dev;

/* One access can fail with no answer from the chip. Try again: the I2C
 * driver resets its state machine after an error. */
static esp_err_t pm1_read_buf(uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < PM1_TRIES; i++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, PM1_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGW(TAG, "read 0x%02x: ok at try %d", reg, i + 1);
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PM1_RETRY_MS));
    }
    return err;
}

static esp_err_t pm1_read(uint8_t reg, uint8_t *val)
{
    return pm1_read_buf(reg, val, 1);
}

/* A voltage register pair in mV. */
static esp_err_t pm1_read_mv(uint8_t reg, int *mv)
{
    uint8_t buf[2];

    ESP_RETURN_ON_ERROR(pm1_read_buf(reg, buf, sizeof(buf)), TAG, "read 0x%02x", reg);
    *mv = buf[0] | (buf[1] << 8);
    return ESP_OK;
}

static esp_err_t pm1_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < PM1_TRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), PM1_TIMEOUT_MS);
        if (err == ESP_OK) {
            if (i > 0) {
                ESP_LOGW(TAG, "write 0x%02x: ok at try %d", reg, i + 1);
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PM1_RETRY_MS));
    }
    ESP_LOGW(TAG, "write 0x%02x: %s", reg, esp_err_to_name(err));
    return err;
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

/* Make a PM1 pin an input. */
static esp_err_t pm1_input(uint8_t pin)
{
    ESP_RETURN_ON_ERROR(pm1_update(REG_GPIO_FUNC0, pin, 0), TAG, "func");
    return pm1_update(REG_GPIO_MODE, pin, 0);
}

esp_err_t pm1_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PM1_ADDR,
        .scl_speed_hz = PM1_HZ,
    };
    i2c_master_bus_handle_t bus = i2cbus_internal();
    uint8_t id = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "no bus");
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
    ESP_RETURN_ON_ERROR(pm1_input(PIN_CHG_STAT), TAG, "charger status");
    /* Let the LCD supply become stable before the LCD reset. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "id 0x%02x, LCD power on", id);
    return ESP_OK;
}

esp_err_t pm1_speaker(bool on)
{
    return pm1_update(REG_GPIO_OUT, PIN_SPEAKER, on ? PIN_SPEAKER : 0);
}

esp_err_t pm1_battery_mv(int *mv)
{
    return pm1_read_mv(REG_VBAT_L, mv);
}

esp_err_t pm1_vin_mv(int *mv)
{
    return pm1_read_mv(REG_VIN_L, mv);
}

esp_err_t pm1_power(bool *usb, bool *charging)
{
    uint8_t src, in;

    ESP_RETURN_ON_ERROR(pm1_read(REG_PWR_SRC, &src), TAG, "power source");
    ESP_RETURN_ON_ERROR(pm1_read(REG_GPIO_IN, &in), TAG, "input");
    *usb = src & PWR_SRC_VIN;
    *charging = !(in & PIN_CHG_STAT);
    return ESP_OK;
}
