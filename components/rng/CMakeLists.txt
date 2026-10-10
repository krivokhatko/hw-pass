idf_component_register(SRCS "rng.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bootloader_support esp_hw_support vault)
