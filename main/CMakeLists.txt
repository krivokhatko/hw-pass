idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES nvs_flash sysinfo pm1 display sound imu vault rng clock usbdev mover keypad ntp cli ui
                       INCLUDE_DIRS "")
