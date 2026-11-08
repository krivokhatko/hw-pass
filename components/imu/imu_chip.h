#pragma once

#include "esp_err.h"

/* The chip driver of the tilt sensor: bmi270.c (StickS3) or mpu6886.c
 * (StickC Plus). imu.c calls these functions. */

/* Start the chip on the internal I2C bus (i2cbus): the accelerometer on,
 * the rest off. */
esp_err_t imu_chip_init(void);

/* Read the acceleration in g, in the axes of the chip. At rest, the axis
 * that points up reads +1. */
esp_err_t imu_chip_read(float g[3]);
