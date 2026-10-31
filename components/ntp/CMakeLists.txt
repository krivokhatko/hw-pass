idf_component_register(SRCS "ntp.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_event esp_netif esp_timer esp_wifi lwip nvs_flash clock rng vault)
