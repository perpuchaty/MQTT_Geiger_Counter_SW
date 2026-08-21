#include <stdio.h>

#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geiger.h"
#include "history_store.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "app";

static void nvs_bringup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_bringup();
    ESP_ERROR_CHECK(settings_init());   /* everything below reads settings_get() */
    ESP_ERROR_CHECK(history_store_init());

    ESP_ERROR_CHECK(board_init());
    board_apply_settings();
    ESP_ERROR_CHECK(geiger_start());
    ESP_ERROR_CHECK(wifi_prov_init());
    ESP_ERROR_CHECK(mqtt_init());

    if (!wifi_has_credentials()) {
        ESP_ERROR_CHECK(wifi_prov_start(WIFI_PROV_BOTH));
        ESP_LOGI(TAG, "provisioning: BluFi over BLE, or join \"%s\"", wifi_softap_ssid());
    }

    for (;;) {
        uint32_t delay_ms = 250 + (esp_random() % 2751);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        board_simulate_tube_pulse();
    }
}
