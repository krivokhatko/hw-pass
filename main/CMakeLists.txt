idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES nvs_flash sysinfo pm1 display vault rng usbdev keypad cli ui
                       INCLUDE_DIRS "")
