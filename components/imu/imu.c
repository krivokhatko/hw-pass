/* BMI270 accelerometer on the internal I2C bus. It tells which edge of
 * the screen is up. Register map from the Bosch data sheet. The chip
 * runs its configuration file (bmi270_config.c): without it, the sensor
 * gives no data. The load takes about 0.2 s at 400 kHz. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pm1.h"
#include "imu.h"

#define IMU_ADDR        0x69    /* SDO high. M5Unified tries this first. */
#define IMU_ADDR_ALT    0x68    /* SDO low */
#define IMU_HZ          400000  /* M5Unified uses 400 kHz for this chip on this bus. */
#define IMU_TIMEOUT_MS  50
#define IMU_TRIES       3       /* I2C attempts for one access: a chip on this bus can miss one. */
#define IMU_RETRY_MS    1

#define REG_CHIP_ID     0x00    /* 0x24 */
#define REG_ERR         0x02    /* Bit 0: fatal. Bits 1-4: internal error. */
#define REG_ACC_X_LSB   0x0C    /* 6 bytes: x, y, z, little endian */
#define REG_INT_STATUS  0x21    /* Bits 0-3: 1 = configuration file OK */
#define REG_ACC_CONF    0x40
#define REG_ACC_RANGE   0x41
#define REG_INIT_CTRL   0x59    /* 0 before the file, 1 after it */
#define REG_INIT_ADDR   0x5B    /* 2 bytes: word index of the next file data */
#define REG_INIT_DATA   0x5E
#define REG_PWR_CONF    0x7C
#define REG_PWR_CTRL    0x7D
#define REG_CMD         0x7E

#define CHIP_ID         0x24
#define CMD_SOFT_RESET  0xB6
#define INIT_OK         0x01
#define ACC_CONF        0x36    /* Low power: 25 Hz, mean of 8 samples */
#define ACC_RANGE_4G    0x01
#define PWR_CTRL_ACC    0x04    /* Accelerometer on; gyroscope, temperature, and aux off */
#define PWR_CONF_SAVE   0x03    /* Advanced power save on, FIFO self wake-up (default) */
#define PWR_CONF_RUN    0x00    /* No power save: for the register writes of the start */
#define LSB_PER_G       8192.0f /* At 4 g */
#define CHUNK           256     /* Bytes of the file in one I2C write */
#define CONFIG_SIZE     8192
#define INIT_WAIT_MS    5       /* Poll interval for the file check */
#define INIT_TRIES      40      /* Bosch: the check takes up to 20 ms. */

/* The dominant axis must have at least this much gravity, else the
 * device lies flat. 0.5 g is 30 degrees from flat. */
#define UP_MIN_G        0.5f

extern const uint8_t bmi270_config_file[CONFIG_SIZE];

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool s_ready;

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

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    i2c_master_transmit_multi_buffer_info_t parts[2] = {
        { .write_buffer = &reg, .buffer_size = 1 },
        { .write_buffer = (uint8_t *)data, .buffer_size = len },
    };
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < IMU_TRIES; i++) {
        err = i2c_master_multi_buffer_transmit(s_dev, parts, 2, IMU_TIMEOUT_MS);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_RETRY_MS));
    }
    ESP_LOGW(TAG, "write 0x%02x: %s", reg, esp_err_to_name(err));
    return err;
}

static esp_err_t reg_write8(uint8_t reg, uint8_t val)
{
    return reg_write(reg, &val, 1);
}

/* Open the chip at one address and read its ID. */
static esp_err_t open(uint8_t addr, uint8_t *id)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = IMU_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(pm1_bus(), &cfg, &s_dev), TAG, "device");
    if (reg_read(REG_CHIP_ID, id, 1) == ESP_OK && *id == CHIP_ID) {
        s_addr = addr;
        return ESP_OK;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

/* Load the configuration file. The chip takes the data in bursts; the
 * index register gives the position of each burst in 16-bit words. */
static esp_err_t upload(void)
{
    for (size_t i = 0; i < CONFIG_SIZE; i += CHUNK) {
        uint8_t index[2] = { (i / 2) & 0x0F, (i / 2) >> 4 };

        ESP_RETURN_ON_ERROR(reg_write(REG_INIT_ADDR, index, 2), TAG, "index %u", (unsigned)i);
        ESP_RETURN_ON_ERROR(reg_write(REG_INIT_DATA, bmi270_config_file + i, CHUNK), TAG, "data %u", (unsigned)i);
    }
    return ESP_OK;
}

esp_err_t imu_init(void)
{
    uint8_t id = 0, status = 0, err_reg = 0;
    esp_err_t err;

    err = open(IMU_ADDR, &id);
    if (err != ESP_OK) {
        err = open(IMU_ADDR_ALT, &id);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "no BMI270 at 0x%02x or 0x%02x (id 0x%02x)", IMU_ADDR, IMU_ADDR_ALT, id);

    /* Bosch: 2 ms after the reset, 450 us after a power mode change. */
    ESP_RETURN_ON_ERROR(reg_write8(REG_CMD, CMD_SOFT_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_CONF, PWR_CONF_RUN), TAG, "power");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(reg_write8(REG_INIT_CTRL, 0x00), TAG, "init start");
    ESP_RETURN_ON_ERROR(upload(), TAG, "file");
    ESP_RETURN_ON_ERROR(reg_write8(REG_INIT_CTRL, 0x01), TAG, "init end");
    for (int i = 0; i < INIT_TRIES; i++) {
        vTaskDelay(pdMS_TO_TICKS(INIT_WAIT_MS));
        ESP_RETURN_ON_ERROR(reg_read(REG_INT_STATUS, &status, 1), TAG, "status");
        if ((status & 0x0F) != 0) {
            break;
        }
    }
    ESP_RETURN_ON_FALSE((status & 0x0F) == INIT_OK, ESP_ERR_INVALID_RESPONSE, TAG,
                        "configuration file refused, status 0x%02x", status);

    ESP_RETURN_ON_ERROR(reg_write8(REG_ACC_CONF, ACC_CONF), TAG, "acc conf");
    ESP_RETURN_ON_ERROR(reg_write8(REG_ACC_RANGE, ACC_RANGE_4G), TAG, "acc range");
    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_CTRL, PWR_CTRL_ACC), TAG, "acc on");
    ESP_RETURN_ON_ERROR(reg_write8(REG_PWR_CONF, PWR_CONF_SAVE), TAG, "power save");
    reg_read(REG_ERR, &err_reg, 1);
    if (err_reg != 0) {
        ESP_LOGW(TAG, "error register 0x%02x", err_reg);
    }
    s_ready = true;
    ESP_LOGI(TAG, "BMI270 at 0x%02x, accelerometer on", s_addr);
    return ESP_OK;
}

bool imu_ready(void)
{
    return s_ready;
}

/* Picture axes from the sensor axes. In the picture without the flip,
 * the USB end of the device is at the right (M5GFX rotation 1 of the
 * portrait reference). The sensor x axis points to the USB end, its y
 * axis to the header edge, its z axis out of the screen (the axis fix of
 * M5Unified for the StickS3: X=+Y, Y=-X, Z=+Z). Thus no sign changes.
 * If the screen turns the wrong way, change the signs here: "info"
 * shows the values. */
esp_err_t imu_read(float g[3])
{
    uint8_t raw[6];
    int16_t x, y, z;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(reg_read(REG_ACC_X_LSB, raw, sizeof(raw)), TAG, "read");
    x = (int16_t)(raw[0] | raw[1] << 8);
    y = (int16_t)(raw[2] | raw[3] << 8);
    z = (int16_t)(raw[4] | raw[5] << 8);
    g[0] = x / LSB_PER_G;
    g[1] = y / LSB_PER_G;
    g[2] = z / LSB_PER_G;
    return ESP_OK;
}

imu_up_t imu_up(void)
{
    float g[3], ax, ay, az;

    if (imu_read(g) != ESP_OK) {
        return IMU_FLAT;
    }
    ax = fabsf(g[0]);
    ay = fabsf(g[1]);
    az = fabsf(g[2]);
    if (ay >= ax && ay > az && ay >= UP_MIN_G) {
        /* A long edge is up. */
        return g[1] > 0 ? IMU_TOP_UP : IMU_BOTTOM_UP;
    }
    if (ax > ay && ax > az && ax >= UP_MIN_G) {
        /* An end is up: the right end of the picture goes down. */
        return g[0] < 0 ? IMU_TOP_UP : IMU_BOTTOM_UP;
    }
    return IMU_FLAT;
}
