idf_component_register(SRCS "blehid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt esp_timer nvs_flash ble rng usbdev)
