idf_component_register(SRCS "config.c"
                            "api.c"
                            "main.c"
                            "lcd.c"
                            "geiger.c"
                            "history_store.c"
                            "settings.c"
                            "mqtt.c"
                            "ota.c"
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
                             spiffs
                             app_update
                             esp_https_ota
                    PRIV_REQUIRES mbedtls)

# IDF 6 NimBLE can miss the Android EspBlufi CCCD event and drop every reply.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_blufi_send_notify")
