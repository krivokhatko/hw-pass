/* AXP192 power chip of the StickC Plus, on the internal I2C bus. It
 * supplies the LCD (LDO3) and the backlight (LDO2), charges the battery,
 * and measures the battery and the USB input. Register map from the
 * AXP192 data sheet; the settings are those of M5Unified for this board. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2cbus.h"
#include "axp192.h"

#define AXP_ADDR        0x34
#define AXP_HZ          400000
#define AXP_TIMEOUT_MS  50
#define AXP_TRIES       3       /* I2C attempts for one access */
#define AXP_RETRY_MS    1

#define REG_POWER_STATUS    0x00    /* Bit 5: the USB input has power. Bit 2: current goes into the battery. */
#define REG_CHARGE_STATUS   0x01    /* Bit 5: a battery is connected. */
#define REG_CHIP_ID         0x03    /* 0x03 */
#define REG_OUTPUTS         0x12    /* Bit 6 EXTEN, bit 3 LDO3, bit 2 LDO2, bit 1 DC-DC3, bit 0 DC-DC1 */
#define REG_LDO23_VOLTAGE   0x28    /* High nibble LDO2, low nibble LDO3: 1.8 V + 0.1 V for each step */
#define REG_VBUS_PATH       0x30    /* Bit 1: current limit on. Bit 0: 0 = 500 mA, 1 = 100 mA. */
#define REG_CHARGE_CTRL     0x33    /* Bit 7 on, bits 6-5 voltage (10 = 4.2 V), bits 3-0 current (0 = 100 mA) */
#define REG_BACKUP_CHARGE   0x35    /* Bit 7: charge the backup battery of the RTC */
#define REG_POWER_KEY       0x36    /* The times of the power key */
#define REG_CHARGE_TEMP     0x39    /* Battery temperature limit of the charge */
#define REG_VBUS_MV         0x5A    /* 12 bits, 1.7 mV for each step */
#define REG_BATTERY_MV      0x78    /* 12 bits, 1.1 mV for each step */
#define REG_ADC_ENABLE      0x82

#define STATUS_VBUS         (1 << 5)
#define STATUS_CHARGING     (1 << 2)
#define STATUS_BATTERY      (1 << 5)
#define OUT_LDO3            (1 << 3)    /* LCD logic, 3.0 V */
#define OUT_LDO2            (1 << 2)    /* LCD backlight, 2.5 to 3.3 V */
#define OUT_DCDC1           (1 << 0)    /* ESP32, 3.3 V. Must stay on. */

#define LDO_3V0             12          /* 1.8 V + 12 x 0.1 V */
#define BL_MIN_STEP         7           /* 2.5 V: the lowest light */
#define BL_MAX_STEP         15          /* 3.3 V */
#define CHARGE_4V2_100MA    0xC0
#define ADC_ALL             0xFF
#define KEY_128MS_4S        0x0C        /* On after 128 ms. A hold of 4 s turns the device off. */
#define VBUS_LIMIT_500MA    0x02
#define TEMP_NO_LIMIT       0xFC        /* The battery has no temperature sensor. */
#define BACKUP_3V0_200UA    0xA2
#define LCD_POWER_MS        100         /* Let the LCD supply become stable before the LCD reset. */

static const char *TAG = "axp192";
static i2c_master_dev_handle_t s_dev;

/* One access can fail with no answer from the chip. Try again: the I2C
 * driver resets its state machine after an error. */
static esp_err_t axp_read_buf(uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < AXP_TRIES; i++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, AXP_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(AXP_RETRY_MS));
    }
    ESP_LOGW(TAG, "read 0x%02x: %s", reg, esp_err_to_name(err));
    return err;
}

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return axp_read_buf(reg, val, 1);
}

/* A 12-bit measurement: 8 high bits, then 4 low bits. */
static esp_err_t axp_read12(uint8_t reg, int *raw)
{
    uint8_t buf[2];

    ESP_RETURN_ON_ERROR(axp_read_buf(reg, buf, sizeof(buf)), TAG, "read 0x%02x", reg);
    *raw = (buf[0] << 4) | (buf[1] & 0x0F);
    return ESP_OK;
}

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < AXP_TRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), AXP_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(AXP_RETRY_MS));
    }
    ESP_LOGW(TAG, "write 0x%02x: %s", reg, esp_err_to_name(err));
    return err;
}

static esp_err_t axp_update(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t val;

    ESP_RETURN_ON_ERROR(axp_read(reg, &val), TAG, "read 0x%02x", reg);
    return axp_write(reg, (val & ~mask) | (bits & mask));
}

esp_err_t axp192_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = AXP_HZ,
    };
    i2c_master_bus_handle_t bus = i2cbus_internal();
    uint8_t id = 0;

    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "no bus");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "device");
    ESP_RETURN_ON_ERROR(axp_read(REG_CHIP_ID, &id), TAG, "no answer at 0x%02x", AXP_ADDR);
    ESP_RETURN_ON_FALSE(id == 0x03, ESP_ERR_NOT_FOUND, TAG, "chip id 0x%02x", id);

    ESP_RETURN_ON_ERROR(axp_write(REG_LDO23_VOLTAGE, LDO_3V0 << 4 | LDO_3V0), TAG, "ldo voltage");
    /* The LCD power on; the backlight off until the view sets it. */
    ESP_RETURN_ON_ERROR(axp_update(REG_OUTPUTS, OUT_DCDC1 | OUT_LDO3 | OUT_LDO2, OUT_DCDC1 | OUT_LDO3), TAG, "outputs");
    ESP_RETURN_ON_ERROR(axp_write(REG_CHARGE_CTRL, CHARGE_4V2_100MA), TAG, "charger");
    ESP_RETURN_ON_ERROR(axp_write(REG_ADC_ENABLE, ADC_ALL), TAG, "adc");
    ESP_RETURN_ON_ERROR(axp_write(REG_POWER_KEY, KEY_128MS_4S), TAG, "power key");
    ESP_RETURN_ON_ERROR(axp_update(REG_VBUS_PATH, 0xFB, VBUS_LIMIT_500MA), TAG, "vbus");
    ESP_RETURN_ON_ERROR(axp_write(REG_CHARGE_TEMP, TEMP_NO_LIMIT), TAG, "charge temperature");
    ESP_RETURN_ON_ERROR(axp_write(REG_BACKUP_CHARGE, BACKUP_3V0_200UA), TAG, "backup battery");
    vTaskDelay(pdMS_TO_TICKS(LCD_POWER_MS));
    ESP_LOGI(TAG, "LCD power on");
    return ESP_OK;
}

esp_err_t axp192_backlight(uint8_t level)
{
    uint8_t step;

    if (level == 0) {
        return axp_update(REG_OUTPUTS, OUT_LDO2, 0);
    }
    step = BL_MIN_STEP + ((BL_MAX_STEP - BL_MIN_STEP) * level + 127) / 255;
    ESP_RETURN_ON_ERROR(axp_update(REG_LDO23_VOLTAGE, 0xF0, step << 4), TAG, "backlight");
    return axp_update(REG_OUTPUTS, OUT_LDO2, OUT_LDO2);
}

esp_err_t axp192_battery_mv(int *mv)
{
    uint8_t status;
    int raw;

    ESP_RETURN_ON_ERROR(axp_read(REG_CHARGE_STATUS, &status), TAG, "status");
    if (!(status & STATUS_BATTERY)) {
        *mv = 0;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(axp_read12(REG_BATTERY_MV, &raw), TAG, "battery");
    *mv = raw * 11 / 10;
    return ESP_OK;
}

esp_err_t axp192_vin_mv(int *mv)
{
    int raw;

    ESP_RETURN_ON_ERROR(axp_read12(REG_VBUS_MV, &raw), TAG, "vbus");
    *mv = raw * 17 / 10;
    return ESP_OK;
}

esp_err_t axp192_power(bool *usb, bool *charging)
{
    uint8_t status;

    ESP_RETURN_ON_ERROR(axp_read(REG_POWER_STATUS, &status), TAG, "power status");
    *usb = status & STATUS_VBUS;
    *charging = status & STATUS_CHARGING;
    return ESP_OK;
}
