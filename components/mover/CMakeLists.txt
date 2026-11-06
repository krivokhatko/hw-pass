idf_component_register(SRCS "mover.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer nvs_flash clock hid sound)
