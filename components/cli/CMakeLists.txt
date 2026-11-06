idf_component_register(SRCS "cli.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_app_format nvs_flash battery blehid clock display hid imu keypad mover ntp rng sound ui usbdev vault web)
