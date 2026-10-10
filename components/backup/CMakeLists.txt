idf_component_register(SRCS "backup.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES mbedtls rng vault)
