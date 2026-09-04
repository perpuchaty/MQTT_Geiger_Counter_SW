#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_app_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool configured;
    bool checking;
    bool update_available;
    bool updating;
    int progress_pct;
    esp_err_t last_error;
    char current_version[sizeof(((esp_app_desc_t *)0)->version)];
    char available_version[sizeof(((esp_app_desc_t *)0)->version)];
} ota_status_t;

esp_err_t ota_init(void);
esp_err_t ota_confirm_running_image(void);
esp_err_t ota_check_on_connect(void);
esp_err_t ota_start_update(void);
void ota_get_status(ota_status_t *status);

#ifdef __cplusplus
}
#endif
