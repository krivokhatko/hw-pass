/* The tilt sensor tells which edge of the screen is up. The chip driver
 * (bmi270.c or mpu6886.c, see imu_chip.h) reads the acceleration in the
 * axes of the chip; this file maps them to the axes of the picture and
 * finds the edge that is up. */
#include <math.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "imu.h"
#include "imu_chip.h"

/* The dominant axis must have at least this much gravity, else the
 * device lies flat. 0.5 g is 30 degrees from flat. */
#define UP_MIN_G        0.5f

static const char *TAG = "imu";
static bool s_ready;

esp_err_t imu_init(void)
{
    ESP_RETURN_ON_ERROR(imu_chip_init(), TAG, "chip");
    s_ready = true;
    return ESP_OK;
}

bool imu_ready(void)
{
    return s_ready;
}

/* Picture axes from the chip axes. In the picture without the flip, the
 * USB end of the device is at the right (M5GFX rotation 1 of the portrait
 * reference). If the screen turns the wrong way, change the signs here:
 * "info" shows the values. */
esp_err_t imu_read(float g[3])
{
    float a[3];

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(imu_chip_read(a), TAG, "read");
#if CONFIG_HWPASS_BOARD_STICKCPLUS
    /* StickC Plus: the chip y axis points along the device, away from the
     * USB end; its x axis across the device, to the side of button B; its
     * z axis out of the screen (the axis figure of M5Stack). The USB end
     * is at the right of the picture, the side of button B at its top. */
    g[0] = -a[1];
    g[1] = a[0];
    g[2] = a[2];
#else
    /* StickS3: the chip x axis points to the USB end, its y axis to the
     * header edge, its z axis out of the screen (the axis fix of M5Unified
     * for the StickS3: X=+Y, Y=-X, Z=+Z). Thus no sign changes. */
    g[0] = a[0];
    g[1] = a[1];
    g[2] = a[2];
#endif
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
