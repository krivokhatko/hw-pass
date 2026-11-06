idf_component_register(SRCS "imu.c" "bmi270_config.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c pm1)
