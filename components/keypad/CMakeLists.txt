idf_component_register(SRCS "keypad.c" "hidmap.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt nvs_flash ble rng)
