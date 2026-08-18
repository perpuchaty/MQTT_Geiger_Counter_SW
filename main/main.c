#include <stdio.h>

#include "config.h"
#include "esp_log.h"
#include "geiger.h"
#include "mqtt.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(geiger_start());
    ESP_ERROR_CHECK(wifi_prov_init());
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(mqtt_init());

    if (!wifi_has_credentials()) {
        ESP_ERROR_CHECK(wifi_prov_start(WIFI_PROV_BOTH));
        ESP_LOGI(TAG, "provisioning: BluFi over BLE, or join \"%s\"", wifi_softap_ssid());
    }
}
