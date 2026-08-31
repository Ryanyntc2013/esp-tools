idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_timer
                                     esp_driver_gpio)
