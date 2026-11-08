# The chip driver differs by board (imu_chip.h). A board without a tilt
# sensor builds the BMI270 driver, and the linker removes it.
if(CONFIG_HWPASS_IMU_MPU6886)
    set(chip "mpu6886.c")
else()
    set(chip "bmi270.c" "bmi270_config.c")
endif()

idf_component_register(SRCS "imu.c" ${chip}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c i2cbus)
