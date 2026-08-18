idf_component_register(SRCS "config.c"
                            "main.c"
                            "geiger.c"
                            "settings.c"
                            "mqtt.c"
                            "wifi_prov.c"
                            "blufi_init.c"
                            "blufi_security.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "index.html"
                    REQUIRES esp_adc
                             esp_driver_gpio
                             esp_driver_spi
                             esp_driver_ledc
                             esp_driver_gptimer
                             esp_timer
                             esp_wifi
                             esp_netif
                             esp_http_server
                             nvs_flash
                             lwip
                             bt
                             app_update
                    PRIV_REQUIRES mbedtls)
