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
} settings_t;

/** Loads NVS contents over the compiled-in defaults. Call once, after nvs_flash_init(). */
esp_err_t settings_init(void);

/** Current values. Never NULL after settings_init(). */
const settings_t *settings_get(void);

/** Persists the given values and makes them current. */
esp_err_t settings_save(const settings_t *in);

/** Restores the compiled-in defaults and clears the NVS namespace. */
esp_err_t settings_reset(void);

#ifdef __cplusplus
}
#endif
