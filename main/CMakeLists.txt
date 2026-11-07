# All modules are in the build for each board. main.c does not start the
# modules of the parts that the board does not have (CONFIG_HWPASS_*), and
# the linker removes their code.
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_pm nvs_flash sysinfo pm1 battery display sound imu vault rng clock usbdev ble blehid hid mover keypad ntp cli ui
                       INCLUDE_DIRS "")
