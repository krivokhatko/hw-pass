idf_component_register(SRCS "cli.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bootloader_support esp_app_format nvs_flash display ui usbdev vault)
