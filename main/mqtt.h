#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MQTT_STATE_DISABLED = 0,
    MQTT_STATE_WAITING,      /* enabled, waiting for the station to get an IP */
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR,
} mqtt_state_t;

/** Starts the publisher task and connects as soon as Wi-Fi and settings allow it. */
esp_err_t mqtt_init(void);

/** Re-reads the settings and restarts the client. Call after settings_save(). */
esp_err_t mqtt_apply(void);

mqtt_state_t mqtt_state(void);
const char *mqtt_state_str(void);

/** Unique per device, e.g. "geigercounter-A1B2C3". Used as the HA object id. */
const char *mqtt_device_id(void);

#ifdef __cplusplus
}
#endif
