/* MPU6886 accelerometer of the StickC Plus on the internal I2C bus.
 * Register map from the TDK InvenSense data sheet. The gyroscope stays
 * off. */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2cbus.h"
#include "imu_chip.h"

#define IMU_ADDR        0x68
#define IMU_HZ          400000
#define IMU_TIMEOUT_MS  50
#define IMU_TRIES       3       /* I2C attempts for one access */
#define IMU_RETRY_MS    1

#define REG_SMPLRT_DIV      0x19    /* Sample rate: 1 kHz / (1 + n) */
#define REG_ACCEL_CONFIG    0x1C    /* Bits 4-3: range */
#define REG_ACCEL_CONFIG2   0x1D    /* Bits 2-0: low-pass filter */
#define REG_ACCEL_XOUT_H    0x3B    /* 6 bytes: x, y, z, big endian */
#define REG_PWR_MGMT_1      0x6B    /* Bit 7: reset. Bit 6: sleep. Bits 2-0: clock. */
#define REG_PWR_MGMT_2      0x6C    /* Bits 5-3: accelerometer axes off. Bits 2-0: gyroscope axes off. */
#define REG_WHO_AM_I        0x75

#define CHIP_ID             0x19
#define RESET               0x80
#define CLOCK_AUTO          0x01    /* Awake, the best clock */
#define GYRO_OFF            0x07
#define RANGE_4G            0x08
#define FILTER_5HZ          0x06    /* Slow changes only: the tilt */
#define RATE_25HZ           39
#define LSB_PER_G           8192.0f /* At 4 g */
#define RESET_MS            10

static const char *TAG = "mpu6886";
static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < IMU_TRIES; i++) {
        err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, IMU_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_RETRY_MS));
    }
    return err;
}

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < IMU_TRIES; i++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), IMU_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_RETRY_MS));
    }
    ESP_LOGW(TAG, "write 0x%02x: %s", reg, esp_err_to_name(err));
    return err;
}

esp_err_t imu_chip_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDR,
        .scl_speed_hz = IMU_HZ,
    };
    i2c_master_bus_handle_t bus = i2cbus_internal();
    uint8_t id = 0;

    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "no bus");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "device");
    ESP_RETURN_ON_ERROR(reg_read(REG_WHO_AM_I, &id, 1), TAG, "no answer at 0x%02x", IMU_ADDR);
    ESP_RETURN_ON_FALSE(id == CHIP_ID, ESP_ERR_NOT_FOUND, TAG, "chip id 0x%02x", id);

    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_MGMT_1, RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(RESET_MS));
    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_MGMT_1, CLOCK_AUTO), TAG, "wake");
    vTaskDelay(pdMS_TO_TICKS(RESET_MS));
    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_MGMT_2, GYRO_OFF), TAG, "gyro off");
    ESP_RETURN_ON_ERROR(reg_write8(REG_ACCEL_CONFIG, RANGE_4G), TAG, "range");
    ESP_RETURN_ON_ERROR(reg_write8(REG_ACCEL_CONFIG2, FILTER_5HZ), TAG, "filter");
    ESP_RETURN_ON_ERROR(reg_write8(REG_SMPLRT_DIV, RATE_25HZ), TAG, "rate");
    ESP_LOGI(TAG, "accelerometer on");
    return ESP_OK;
}

esp_err_t imu_chip_read(float g[3])
{
    uint8_t raw[6];
    int16_t x, y, z;

    ESP_RETURN_ON_ERROR(reg_read(REG_ACCEL_XOUT_H, raw, sizeof(raw)), TAG, "read");
    x = (int16_t)(raw[0] << 8 | raw[1]);
    y = (int16_t)(raw[2] << 8 | raw[3]);
    z = (int16_t)(raw[4] << 8 | raw[5]);
    g[0] = x / LSB_PER_G;
    g[1] = y / LSB_PER_G;
    g[2] = z / LSB_PER_G;
    return ESP_OK;
}
