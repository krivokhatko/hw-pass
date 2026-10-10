idf_component_register(SRCS "web.c" "dns.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "www/index.html"
                       PRIV_REQUIRES esp_app_format esp_event esp_http_server esp_netif esp_timer
                                     esp_wifi lwip backup rng vault)
