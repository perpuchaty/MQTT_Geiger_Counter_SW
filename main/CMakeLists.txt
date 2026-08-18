idf_component_register(SRCS "config.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_adc
                             esp_driver_gpio
                             esp_driver_spi
                             esp_driver_ledc
                             esp_driver_gptimer
                             esp_timer
                             app_update)
