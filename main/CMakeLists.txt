idf_component_register(SRCS "main.c" "bin_calendar.c" "bin_fetch.c" "display.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client mbedtls
                             esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc)
