#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The internal I2C bus of the board. StickS3: the PM1 power chip, the
 * BMI270 tilt sensor, and the ES8311 audio codec. StickC Plus: the AXP192
 * power chip and the MPU6886 tilt sensor. The first call makes the bus:
 * call it from app_main first, before other tasks run. NULL when the bus
 * does not start. */
i2c_master_bus_handle_t i2cbus_internal(void);

#ifdef __cplusplus
}
#endif
