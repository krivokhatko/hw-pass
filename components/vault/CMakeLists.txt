idf_component_register(SRCS "vault.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES efuse esp_hw_support mbedtls nvs_flash kdf)
