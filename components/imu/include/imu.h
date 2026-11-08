#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which edge of the picture is up, from the direction of gravity. The
 * picture is the landscape frame without the flip. */
typedef enum {
    IMU_FLAT,       /* The device lies flat, or the sensor does not work. */
    IMU_TOP_UP,     /* The top edge of the picture is up: no flip. */
    IMU_BOTTOM_UP,  /* The bottom edge is up: the picture needs the flip. */
} imu_up_t;

/* Start the tilt sensor of the board (BMI270 or MPU6886): the
 * accelerometer on at 25 Hz, the rest off. Call after the power chip. */
esp_err_t imu_init(void);

/* True when the sensor started. */
bool imu_ready(void);

/* Read the acceleration in g, in the picture axes: x to the right, y to
 * the top, z out of the screen. At rest, the axis that points up reads
 * +1. Safe to call from any task. */
esp_err_t imu_read(float g[3]);

/* Which edge of the picture is up now: one read of the sensor. When the
 * device stands on an end, the end at the right of the picture goes
 * down: the text reads from the top to the bottom. */
imu_up_t imu_up(void);

#ifdef __cplusplus
}
#endif
