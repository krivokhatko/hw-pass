idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash qrcode battery blehid buttons clock display hid imu keypad mover ntp sound usbdev vault web)
