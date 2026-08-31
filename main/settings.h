#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every value persisted in NVS lives in settings_t. Adding a field means:
 * add it here, add its default in settings_defaults() and add a row to the
 * descriptor table in settings.c (NVS keys are limited to 15 characters). */

#define SETTINGS_NAME_LEN   32
#define SETTINGS_URI_LEN    128
#define SETTINGS_USER_LEN   48
#define SETTINGS_PASS_LEN   64
#define SETTINGS_TOPIC_LEN  48

/* Accepted ranges, enforced by settings_save(). */
#define TUBE_WINDOW_MIN_S   5
#define TUBE_WINDOW_MAX_S   300
#define TUBE_TARGET_MIN_V   200
#define TUBE_TARGET_MAX_V   600
#define TUBE_CPM_MIN        1.0f
#define TUBE_CPM_MAX        10000.0f
#define HISTORY_SHORT_MIN_S 60
#define HISTORY_SHORT_MAX_S 1728000

/** Click length emitted by the speaker on every tube pulse. */
typedef enum {
    SOUND_SHORT = 0,
    SOUND_NORMAL,
    SOUND_LONG,
    SOUND_TYPE_COUNT,
} sound_type_t;

typedef struct {
    /* Identity */
    char device_name[SETTINGS_NAME_LEN];   /* shown in Home Assistant and the web UI */

    /* MQTT / Home Assistant */
    bool     mqtt_enabled;
    char     mqtt_uri[SETTINGS_URI_LEN];   /* mqtt://host:1883, mqtts://..., ws://... */
    char     mqtt_user[SETTINGS_USER_LEN];
    char     mqtt_pass[SETTINGS_PASS_LEN];
    char     mqtt_topic[SETTINGS_TOPIC_LEN];      /* base topic, device id is appended */
    bool     mqtt_discovery;                      /* publish Home Assistant discovery */
    char     mqtt_ha_prefix[SETTINGS_TOPIC_LEN];  /* discovery prefix, usually "homeassistant" */
    uint16_t mqtt_interval_s;                     /* state publish period */

    /* Tube / measurement */
    uint16_t tube_window_s;      /* sliding window used to derive counts per minute */
    float    tube_cpm_per_usvh;  /* tube sensitivity: CPM equal to 1 uSv/h */
    uint16_t tube_target_v;      /* HV target the boost converter regulates to */

    /* Speaker */
    uint8_t  spk_volume;         /* 0-200 PWM scale, loudest at 100 */
    uint8_t  spk_sound;          /* sound_type_t */

    /* Power */
    bool     batt_charge_en;

    /* High voltage */
    bool     hv_start_enabled;   /* enable the tube HV supply at boot */

    /* Indicators */
    bool     led_enabled;
    uint8_t  lcd_brightness;     /* 0-100 */
    bool     lcd_auto_dim;       /* dim the backlight when no button is pressed */

    /* Web UI history preview on the Live tab */
    uint32_t history_short_window_s;
} settings_t;

/** Loads NVS contents over the compiled-in defaults. Call once, after nvs_flash_init(). */
esp_err_t settings_init(void);

/** Current values. Never NULL after settings_init(). */
const settings_t *settings_get(void);

/** Persists the given values and makes them current. Out of range fields are clamped. */
esp_err_t settings_save(const settings_t *in);

/** Restores the compiled-in defaults and clears the NVS namespace. */
esp_err_t settings_reset(void);

#ifdef __cplusplus
}
#endif
