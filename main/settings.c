#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define SETTINGS_NAMESPACE "geiger"

typedef enum {
    T_BOOL,
    T_U8,
    T_I16,
    T_U16,
    T_U32,
    T_FLOAT,
    T_STR,
} setting_type_t;

typedef struct {
    const char    *key;   /* NVS keys are limited to 15 characters */
    setting_type_t type;
    size_t         offset;
    size_t         size;
    bool           secret; /* never printed in clear */
} setting_desc_t;

#define S_FIELD(k, t, f)  { k, t, offsetof(settings_t, f), sizeof(((settings_t *)0)->f), false }
#define S_SECRET(k, t, f) { k, t, offsetof(settings_t, f), sizeof(((settings_t *)0)->f), true }

static const setting_desc_t s_desc[] = {
    S_FIELD("dev_name",   T_STR,   device_name),
    S_FIELD("mq_en",      T_BOOL,  mqtt_enabled),
    S_FIELD("mq_uri",     T_STR,   mqtt_uri),
    S_FIELD("mq_user",    T_STR,   mqtt_user),
    S_SECRET("mq_pass",   T_STR,   mqtt_pass),
    S_FIELD("mq_topic",   T_STR,   mqtt_topic),
    S_FIELD("mq_disc",    T_BOOL,  mqtt_discovery),
    S_FIELD("mq_ha_pfx",  T_STR,   mqtt_ha_prefix),
    S_FIELD("mq_period",  T_U16,   mqtt_interval_s),
    S_FIELD("tube_win",   T_U16,   tube_window_s),
    S_FIELD("tube_cpm",   T_FLOAT, tube_cpm_per_usvh),
    S_FIELD("tube_v",     T_U16,   tube_target_v),
    S_FIELD("spk_vol",    T_U8,    spk_volume),
    S_FIELD("spk_snd",    T_U8,    spk_sound),
    S_FIELD("chg_en",     T_BOOL,  batt_charge_en),
    S_FIELD("hv_start",   T_BOOL,  hv_start_enabled),
    S_FIELD("led_en",     T_BOOL,  led_enabled),
    S_FIELD("lcd_bri",    T_U8,    lcd_brightness),
    S_FIELD("lcd_dim",    T_BOOL,  lcd_auto_dim),
    S_FIELD("tz_offset",  T_I16,   timezone_offset_min),
    S_FIELD("tz_dst",     T_BOOL,  daylight_saving),
    S_FIELD("hist_win",   T_U32,   history_short_window_s),
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

    c->tube_window_s     = CONFIG_GEIGER_TUBE_WINDOW_S;
    c->tube_cpm_per_usvh = CONFIG_GEIGER_TUBE_CPM_PER_USVH_X10 / 10.0f;
    c->tube_target_v     = CONFIG_GEIGER_TUBE_TARGET_V;

    c->spk_volume     = CONFIG_GEIGER_SPK_VOLUME;
    c->spk_sound      = CONFIG_GEIGER_SPK_SOUND;
    c->lcd_brightness = CONFIG_GEIGER_LCD_BRIGHTNESS;
    c->history_short_window_s = CONFIG_GEIGER_HISTORY_SHORT_WINDOW_S;
#ifdef CONFIG_GEIGER_LCD_AUTO_DIM
    c->lcd_auto_dim = true;
#endif
#ifdef CONFIG_GEIGER_BATT_CHARGE_EN
    c->batt_charge_en = true;
#endif
#ifdef CONFIG_GEIGER_LED_ENABLED
    c->led_enabled = true;
#endif
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void settings_clamp(settings_t *c)
{
    c->tube_window_s = clamp_u16(c->tube_window_s, TUBE_WINDOW_MIN_S, TUBE_WINDOW_MAX_S);
    c->tube_target_v = clamp_u16(c->tube_target_v, TUBE_TARGET_MIN_V, TUBE_TARGET_MAX_V);
    c->mqtt_interval_s = clamp_u16(c->mqtt_interval_s, 1, 3600);
    c->history_short_window_s = clamp_u32(c->history_short_window_s,
                                          HISTORY_SHORT_MIN_S,
                                          HISTORY_SHORT_MAX_S);

    if (!(c->tube_cpm_per_usvh >= TUBE_CPM_MIN)) {
        c->tube_cpm_per_usvh = TUBE_CPM_MIN;   /* also catches NaN */
    } else if (c->tube_cpm_per_usvh > TUBE_CPM_MAX) {
        c->tube_cpm_per_usvh = TUBE_CPM_MAX;
    }
    if (c->spk_volume > 200) {
        c->spk_volume = 200;
    }
    if (c->lcd_brightness > 100) {
        c->lcd_brightness = 100;
    }
    if (c->spk_sound >= SOUND_TYPE_COUNT) {
        c->spk_sound = SOUND_NORMAL;
    }
    if (c->timezone_offset_min < TIMEZONE_OFFSET_MIN) {
        c->timezone_offset_min = TIMEZONE_OFFSET_MIN;
    } else if (c->timezone_offset_min > TIMEZONE_OFFSET_MAX) {
        c->timezone_offset_min = TIMEZONE_OFFSET_MAX;
    }
    c->timezone_offset_min = (c->timezone_offset_min / 30) * 30;
}

static void settings_apply_timezone(void)
{
    int offset_min = s_cfg.timezone_offset_min + (s_cfg.daylight_saving ? 60 : 0);
    int absolute_min = offset_min < 0 ? -offset_min : offset_min;
    char tz[20];
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", offset_min > 0 ? '-' : '+',
             absolute_min / 60, absolute_min % 60);
    setenv("TZ", tz, 1);
    tzset();
}

static void log_field(const char *action, const setting_desc_t *d, const void *field)
{
    switch (d->type) {
    case T_BOOL:
        ESP_LOGI(TAG, "%s %-9s = %s", action, d->key, *(const bool *)field ? "true" : "false");
        break;
    case T_U8:
        ESP_LOGI(TAG, "%s %-9s = %u", action, d->key, (unsigned)*(const uint8_t *)field);
        break;
    case T_I16:
        ESP_LOGI(TAG, "%s %-9s = %d", action, d->key, (int)*(const int16_t *)field);
        break;
    case T_U16:
        ESP_LOGI(TAG, "%s %-9s = %u", action, d->key, (unsigned)*(const uint16_t *)field);
        break;
    case T_U32:
        ESP_LOGI(TAG, "%s %-9s = %lu", action, d->key, (unsigned long)*(const uint32_t *)field);
        break;
    case T_FLOAT: {
        /* printed as x.y, the log formatter has no float support with nano libc */
        int scaled = (int)(*(const float *)field * 10.0f + 0.5f);
        ESP_LOGI(TAG, "%s %-9s = %d.%d", action, d->key, scaled / 10, scaled % 10);
        break;
    }
    default: {
        const char *str = field;
        ESP_LOGI(TAG, "%s %-9s = %s", action, d->key,
                 d->secret ? (str[0] ? "<set>" : "<empty>") : str);
        break;
    }
    }
}

static void settings_dump(const char *action, const settings_t *c)
{
    for (int i = 0; i < sizeof(s_desc) / sizeof(s_desc[0]); i++) {
        log_field(action, &s_desc[i], (const uint8_t *)c + s_desc[i].offset);
    }
}

static int settings_load(nvs_handle_t nvs, settings_t *c)
{
    int found = 0;

    for (int i = 0; i < sizeof(s_desc) / sizeof(s_desc[0]); i++) {
        const setting_desc_t *d = &s_desc[i];
        void *field = (uint8_t *)c + d->offset;
        esp_err_t err;

        switch (d->type) {
        case T_BOOL: {
            uint8_t v;
            err = nvs_get_u8(nvs, d->key, &v);
            if (err == ESP_OK) {
                *(bool *)field = v != 0;
            }
            break;
        }
        case T_U8:
            err = nvs_get_u8(nvs, d->key, (uint8_t *)field);
            break;
        case T_I16:
            err = nvs_get_i16(nvs, d->key, (int16_t *)field);
            break;
        case T_U16:
            err = nvs_get_u16(nvs, d->key, (uint16_t *)field);
            break;
        case T_U32:
            err = nvs_get_u32(nvs, d->key, (uint32_t *)field);
            break;
        case T_FLOAT: {
            uint32_t bits;
            err = nvs_get_u32(nvs, d->key, &bits);
            if (err == ESP_OK) {
                memcpy(field, &bits, sizeof(bits));
            }
            break;
        }
        default: {
            size_t len = d->size;
            err = nvs_get_str(nvs, d->key, (char *)field, &len);
            break;
        }
        }

        if (err == ESP_OK) {
            found++;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "read %s failed: %s", d->key, esp_err_to_name(err));
        }
    }
    return found;
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
        case T_U8:
            err = nvs_set_u8(nvs, d->key, *(const uint8_t *)field);
            break;
        case T_I16:
            err = nvs_set_i16(nvs, d->key, *(const int16_t *)field);
            break;
        case T_U16:
            err = nvs_set_u16(nvs, d->key, *(const uint16_t *)field);
            break;
        case T_U32:
            err = nvs_set_u32(nvs, d->key, *(const uint32_t *)field);
            break;
        case T_FLOAT: {
            uint32_t bits;
            memcpy(&bits, field, sizeof(bits));
            err = nvs_set_u32(nvs, d->key, bits);
            break;
        }
        default:
            err = nvs_set_str(nvs, d->key, (const char *)field);
            break;
        }
        ESP_RETURN_ON_ERROR(err, TAG, "store %s", d->key);
        log_field("store", d, field);
    }
    return nvs_commit(nvs);
}

esp_err_t settings_init(void)
{
    settings_defaults(&s_cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using compiled-in defaults");
        settings_dump("default", &s_cfg);
        settings_apply_timezone();
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");

    int found = settings_load(nvs, &s_cfg);
    nvs_close(nvs);
    settings_clamp(&s_cfg);

    ESP_LOGI(TAG, "restored %d of %d keys from NVS", found, (int)(sizeof(s_desc) / sizeof(s_desc[0])));
    settings_dump("load ", &s_cfg);
    settings_apply_timezone();
    return ESP_OK;
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

esp_err_t settings_save(const settings_t *in)
{
    ESP_RETURN_ON_FALSE(in, ESP_ERR_INVALID_ARG, TAG, "null settings");

    settings_t cfg = *in;
    settings_clamp(&cfg);

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open");

    esp_err_t err = settings_store(nvs, &cfg);
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "commit");

    s_cfg = cfg;
    settings_apply_timezone();
    ESP_LOGI(TAG, "settings committed to NVS");
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
    settings_apply_timezone();
    ESP_LOGI(TAG, "settings erased, back to defaults");
    settings_dump("default", &s_cfg);
    return ESP_OK;
}
