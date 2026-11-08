idf_component_register(SRCS "battery.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer esp_adc axp192 pm1 rng)
