#include "mqtt.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "geiger.h"
#include "lcd.h"
#include "mqtt_client.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "mqtt";

#define MQTT_BASE_LEN       160
#define MQTT_TOPIC_LEN      (MQTT_BASE_LEN + 32)   /* base + "/state", "/status", ... */
#define MQTT_QOS            0
#define MQTT_KEEPALIVE_S    30

static esp_mqtt_client_handle_t s_client;
static mqtt_state_t             s_state;
static SemaphoreHandle_t        s_lock;

static char s_dev_id[32];
static char s_base[MQTT_BASE_LEN];
static char s_state_topic[MQTT_TOPIC_LEN];
static char s_avail_topic[MQTT_TOPIC_LEN];

/* =========================================================================
 * Home Assistant MQTT discovery
 * ====================================================================== */

typedef struct {
    const char *id;             /* object id suffix and JSON key in the state payload */
    const char *name;
    const char *unit;
    const char *device_class;   /* NULL when Home Assistant has no matching class */
    const char *state_class;
    const char *icon;
} ha_sensor_t;

static const ha_sensor_t s_sensors[] = {
    { "usvh",  "Dose rate",     "µSv/h", NULL,      "measurement",      "mdi:radioactive"      },
    { "cpm",   "Counts",        "CPM",   NULL,      "measurement",      "mdi:radioactive"      },
    { "total", "Total counts",  "counts", NULL,     "total_increasing", "mdi:counter"          },
    { "hv",    "Tube voltage",  "V",     "voltage", "measurement",      NULL                   },
    { "vbat",  "Battery",       "V",     "voltage", "measurement",      NULL                   },
    { "rssi",  "Wi-Fi signal",  "dBm",   "signal_strength", "measurement", NULL                },
};

static void ha_add_device(cJSON *root)
{
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_dev_id));
    cJSON_AddStringToObject(dev, "name", settings_get()->device_name);
    cJSON_AddStringToObject(dev, "manufacturer", "DIY");
    cJSON_AddStringToObject(dev, "model", "ESP32-C6 Geiger Counter");
    cJSON_AddStringToObject(dev, "sw_version", app->version);
    cJSON_AddStringToObject(dev, "configuration_url", "http://" WIFI_MDNS_HOSTNAME ".local");
}

static void ha_publish_discovery(void)
{
    const settings_t *cfg = settings_get();
    char topic[MQTT_TOPIC_LEN];
    char buf[96];

    for (int i = 0; i < sizeof(s_sensors) / sizeof(s_sensors[0]); i++) {
        const ha_sensor_t *s = &s_sensors[i];

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", s->name);

        snprintf(buf, sizeof(buf), "%s_%s", s_dev_id, s->id);
        cJSON_AddStringToObject(root, "unique_id", buf);
        cJSON_AddStringToObject(root, "object_id", buf);

        cJSON_AddStringToObject(root, "state_topic", s_state_topic);
        cJSON_AddStringToObject(root, "availability_topic", s_avail_topic);

        snprintf(buf, sizeof(buf), "{{ value_json.%s }}", s->id);
        cJSON_AddStringToObject(root, "value_template", buf);

        cJSON_AddStringToObject(root, "unit_of_measurement", s->unit);
        cJSON_AddStringToObject(root, "state_class", s->state_class);
        if (s->device_class) {
            cJSON_AddStringToObject(root, "device_class", s->device_class);
        }
        if (s->icon) {
            cJSON_AddStringToObject(root, "icon", s->icon);
        }
        ha_add_device(root);

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!payload) {
            continue;
        }
        snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", cfg->mqtt_ha_prefix, s_dev_id, s->id);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
        cJSON_free(payload);
    }
    ESP_LOGI(TAG, "published discovery for %d entities", (int)(sizeof(s_sensors) / sizeof(s_sensors[0])));
}

/* =========================================================================
 * State publishing
 * ====================================================================== */

static void mqtt_publish_state(void)
{
    int hv_mv = 0;
    int vbat_mv = 0;
    wifi_ap_record_t ap = {0};

    board_tube_voltage_get_mv(&hv_mv);
    board_adc_get_mv(BOARD_ADC_VLATCH, &vbat_mv);
    esp_wifi_sta_get_ap_info(&ap);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "usvh", geiger_usvh());
    cJSON_AddNumberToObject(root, "cpm", geiger_cpm());
    cJSON_AddNumberToObject(root, "total", geiger_total());
    cJSON_AddNumberToObject(root, "hv", hv_mv / 1000.0);
    cJSON_AddNumberToObject(root, "vbat", vbat_mv / 1000.0);
    cJSON_AddNumberToObject(root, "rssi", ap.rssi);
    cJSON_AddBoolToObject(root, "hv_ok", board_input_level(BOARD_IN_VTUBE_OK));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload) {
        esp_mqtt_client_publish(s_client, s_state_topic, payload, 0, MQTT_QOS, false);
        cJSON_free(payload);
    }
}

static void mqtt_task(void *arg)
{
    for (;;) {
        uint16_t period = settings_get()->mqtt_interval_s;
        vTaskDelay(pdMS_TO_TICKS((period ? period : 30) * 1000));

        if (s_state == MQTT_STATE_CONNECTED && s_client) {
            mqtt_publish_state();
        }
    }
}

/* =========================================================================
 * Client lifecycle
 * ====================================================================== */

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_state = MQTT_STATE_CONNECTED;
        ESP_LOGI(TAG, "connected to %s", settings_get()->mqtt_uri);
        esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, true);
        if (settings_get()->mqtt_discovery) {
            ha_publish_discovery();
        }
        mqtt_publish_state();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_state = MQTT_STATE_CONNECTING;
        ESP_LOGW(TAG, "disconnected");
        break;

    case MQTT_EVENT_ERROR:
        s_state = MQTT_STATE_ERROR;
        ESP_LOGW(TAG, "error, transport %d", event->error_handle->error_type);
        break;

    default:
        break;
    }
    lcd_refresh();
}

static void mqtt_client_stop(void)
{
    if (!s_client) {
        return;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
}

static esp_err_t mqtt_client_start(void)
{
    const settings_t *cfg = settings_get();

    snprintf(s_base, sizeof(s_base), "%s/%s", cfg->mqtt_topic, s_dev_id);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", s_base);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/status", s_base);

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = cfg->mqtt_uri,
        .credentials = {
            .client_id = s_dev_id,
            .username  = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
            .authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        },
        .session = {
            .keepalive = MQTT_KEEPALIVE_S,
            .last_will = {
                .topic  = s_avail_topic,
                .msg    = "offline",
                .qos    = 1,
                .retain = 1,
            },
        },
    };

    s_client = esp_mqtt_client_init(&mcfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "client init");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL),
                        TAG, "register events");

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        mqtt_client_stop();
        s_state = MQTT_STATE_ERROR;
        return err;
    }
    s_state = MQTT_STATE_CONNECTING;
    ESP_LOGI(TAG, "connecting to %s, base topic %s", cfg->mqtt_uri, s_base);
    return ESP_OK;
}

esp_err_t mqtt_apply(void)
{
    const settings_t *cfg = settings_get();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mqtt_client_stop();

    esp_err_t err = ESP_OK;
    if (!cfg->mqtt_enabled || cfg->mqtt_uri[0] == '\0') {
        s_state = MQTT_STATE_DISABLED;
    } else if (!wifi_is_connected()) {
        s_state = MQTT_STATE_WAITING;
    } else {
        err = mqtt_client_start();
    }
    xSemaphoreGive(s_lock);
    lcd_refresh();
    return err;
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    mqtt_apply();
}

esp_err_t mqtt_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof(s_dev_id), "%s-%02X%02X%02X",
             WIFI_MDNS_HOSTNAME, mac[3], mac[4], mac[5]);

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL),
                        TAG, "ip events");
    ESP_RETURN_ON_FALSE(xTaskCreate(mqtt_task, "mqtt_pub", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "publisher task");

    return mqtt_apply();
}

mqtt_state_t mqtt_state(void)
{
    return s_state;
}

const char *mqtt_state_str(void)
{
    switch (s_state) {
    case MQTT_STATE_DISABLED:   return "disabled";
    case MQTT_STATE_WAITING:    return "waiting for Wi-Fi";
    case MQTT_STATE_CONNECTING: return "connecting";
    case MQTT_STATE_CONNECTED:  return "connected";
    default:                    return "error";
    }
}

const char *mqtt_device_id(void)
{
    return s_dev_id;
}
