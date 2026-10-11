idf_component_register(SRCS "cli.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_app_format nvs_flash display keypad rng sound ui usbdev vault web)
