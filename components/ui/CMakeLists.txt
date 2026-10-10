idf_component_register(SRCS "ui.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash qrcode buttons display keypad usbdev vault web)
