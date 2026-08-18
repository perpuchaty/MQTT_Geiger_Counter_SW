#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define SETTINGS_NAMESPACE "geiger"

typedef enum {
    T_BOOL,
    T_U16,
    T_STR,
} setting_type_t;

typedef struct {
    const char    *key;   /* NVS keys are limited to 15 characters */
    setting_type_t type;
    size_t         offset;
    size_t         size;
} setting_desc_t;

#define S_FIELD(k, t, f) { k, t, offsetof(settings_t, f), sizeof(((settings_t *)0)->f) }

static const setting_desc_t s_desc[] = {
    S_FIELD("dev_name",   T_STR,  device_name),
    S_FIELD("mq_en",      T_BOOL, mqtt_enabled),
    S_FIELD("mq_uri",     T_STR,  mqtt_uri),
    S_FIELD("mq_user",    T_STR,  mqtt_user),
    S_FIELD("mq_pass",    T_STR,  mqtt_pass),
    S_FIELD("mq_topic",   T_STR,  mqtt_topic),
    S_FIELD("mq_disc",    T_BOOL, mqtt_discovery),
    S_FIELD("mq_ha_pfx",  T_STR,  mqtt_ha_prefix),
    S_FIELD("mq_period",  T_U16,  mqtt_interval_s),
};

static settings_t s_cfg;

static void settings_defaults(settings_t *c)
{
    memset(c, 0, sizeof(*c));

    strlcpy(c->device_name, CONFIG_GEIGER_DEVICE_NAME, sizeof(c->device_name));

#ifdef CONFIG_GEIGER_MQTT_ENABLED
    c->mqtt_enabled = true;
#endif
#ifdef CONFIG_GEIGER_MQTT_HA_DISCOVERY
    c->mqtt_discovery = true;
#endif
    c->mqtt_interval_s = CONFIG_GEIGER_MQTT_INTERVAL_S;
    strlcpy(c->mqtt_uri, CONFIG_GEIGER_MQTT_URI, sizeof(c->mqtt_uri));
    strlcpy(c->mqtt_user, CONFIG_GEIGER_MQTT_USER, sizeof(c->mqtt_user));
    strlcpy(c->mqtt_pass, CONFIG_GEIGER_MQTT_PASS, sizeof(c->mqtt_pass));
    strlcpy(c->mqtt_topic, CONFIG_GEIGER_MQTT_TOPIC, sizeof(c->mqtt_topic));
    strlcpy(c->mqtt_ha_prefix, CONFIG_GEIGER_MQTT_HA_PREFIX, sizeof(c->mqtt_ha_prefix));
}

static void settings_load(nvs_handle_t nvs, settings_t *c)
{
    for (int i = 0; i < sizeof(s_desc) / sizeof(s_desc[0]); i++) {
        const setting_desc_t *d = &s_desc[i];
        void *field = (uint8_t *)c + d->offset;

        switch (d->type) {
        case T_BOOL: {
            uint8_t v;
            if (nvs_get_u8(nvs, d->key, &v) == ESP_OK) {
                *(bool *)field = v != 0;
            }
            break;
        }
        case T_U16:
            nvs_get_u16(nvs, d->key, (uint16_t *)field);
            break;
        case T_STR: {
            size_t len = d->size;
            nvs_get_str(nvs, d->key, (char *)field, &len);
            break;
        }
        }
    }
}

static esp_err_t settings_store(nvs_handle_t nvs, const settings_t *c)
{
    for (int i = 0; i < sizeof(s_desc) / sizeof(s_desc[0]); i++) {
        const setting_desc_t *d = &s_desc[i];
        const void *field = (const uint8_t *)c + d->offset;
        esp_err_t err;

        switch (d->type) {
        case T_BOOL:
            err = nvs_set_u8(nvs, d->key, *(const bool *)field ? 1 : 0);
            break;
        case T_U16:
            err = nvs_set_u16(nvs, d->key, *(const uint16_t *)field);
            break;
        default:
            err = nvs_set_str(nvs, d->key, (const char *)field);
            break;
        }
        ESP_RETURN_ON_ERROR(err, TAG, "store %s", d->key);
    }
    return nvs_commit(nvs);
}

esp_err_t settings_init(void)
{
    settings_defaults(&s_cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");

    settings_load(nvs, &s_cfg);
    nvs_close(nvs);
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

esp_err_t settings_save(const settings_t *in)
{
    ESP_RETURN_ON_FALSE(in, ESP_ERR_INVALID_ARG, TAG, "null settings");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");

    esp_err_t err = settings_store(nvs, in);
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "commit");

    s_cfg = *in;
    ESP_LOGI(TAG, "settings saved");
    return ESP_OK;
}

esp_err_t settings_reset(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");

    esp_err_t err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "erase");

    settings_defaults(&s_cfg);
    return ESP_OK;
}
