idf_component_register(SRCS "usbdev.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_tinyusb esp_hw_support esp_timer hal nvs_flash soc)
