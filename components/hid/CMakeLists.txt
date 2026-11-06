idf_component_register(SRCS "hid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer esp_tinyusb nvs_flash blehid usbdev)
