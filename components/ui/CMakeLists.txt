# The view differs by board: the LCD with the tilt sensor, or the key LEDs.
# The requirements do not depend on the configuration: ESP-IDF reads them
# before it has the configuration. The linker removes the unused module.
if(CONFIG_HWPASS_LCD)
    set(view "view_lcd.c")
else()
    set(view "view_led.c")
endif()

idf_component_register(SRCS "ui.c" ${view}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash qrcode battery blehid buttons clock serial display hid imu keypad leds mover ntp sound usbdev vault web)
