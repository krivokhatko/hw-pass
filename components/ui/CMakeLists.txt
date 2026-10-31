idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash qrcode buttons clock display keypad mover ntp sound usbdev vault web)
