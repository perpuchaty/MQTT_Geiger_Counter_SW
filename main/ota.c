#include "ota.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "ota";

#define OTA_NAMESPACE "ota"
#define OTA_TASK_STACK 8192

static SemaphoreHandle_t s_lock;
static ota_status_t s_status;

static void status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void store_available(const char *version)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(OTA_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot open OTA state: %s", esp_err_to_name(err));
        return;
    }

    if (version && version[0]) {
        err = nvs_set_str(nvs, "available", version);
    } else {
        err = nvs_erase_key(nvs, "available");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save OTA state: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

static void load_available(void)
{
    nvs_handle_t nvs;
    if (nvs_open(OTA_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_status.available_version);
    if (nvs_get_str(nvs, "available", s_status.available_version, &len) == ESP_OK) {
        s_status.update_available = s_status.available_version[0] != '\0';
    }
    nvs_close(nvs);
}

static esp_http_client_config_t http_config(void)
{
    esp_http_client_config_t config = {
        .url = CONFIG_GEIGER_OTA_URL,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    return config;
}

static esp_err_t open_image(esp_https_ota_handle_t *handle, esp_app_desc_t *description)
{
    esp_http_client_config_t client_config = http_config();
    esp_https_ota_config_t ota_config = {
        .http_config = &client_config,
    };

    esp_err_t err = esp_https_ota_begin(&ota_config, handle);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_https_ota_get_img_desc(*handle, description);
    if (err != ESP_OK) {
        esp_https_ota_abort(*handle);
        *handle = NULL;
    }
    return err;
}

static int compare_versions(const char *candidate, const char *current)
{
    if (*candidate == 'v' || *candidate == 'V') candidate++;
    if (*current == 'v' || *current == 'V') current++;

    while (isdigit((unsigned char)*candidate) || isdigit((unsigned char)*current)) {
        char *candidate_end;
        char *current_end;
        unsigned long candidate_part = strtoul(candidate, &candidate_end, 10);
        unsigned long current_part = strtoul(current, &current_end, 10);
        if (candidate_part != current_part) {
            return candidate_part > current_part ? 1 : -1;
        }
        candidate = *candidate_end == '.' ? candidate_end + 1 : candidate_end;
        current = *current_end == '.' ? current_end + 1 : current_end;
    }

    bool candidate_prerelease = *candidate == '-';
    bool current_prerelease = *current == '-';
    if (candidate_prerelease != current_prerelease) {
        return candidate_prerelease ? -1 : 1;
    }
    return strcmp(candidate, current);
}

static void check_task(void *arg)
{
    esp_https_ota_handle_t handle = NULL;
    esp_app_desc_t candidate = {0};
    esp_err_t err = open_image(&handle, &candidate);
    if (handle) {
        esp_https_ota_abort(handle);
    }

    bool available = err == ESP_OK && compare_versions(candidate.version, s_status.current_version) > 0;
    status_lock();
    s_status.checking = false;
    s_status.last_error = err;
    s_status.update_available = available;
    if (available) {
        strlcpy(s_status.available_version, candidate.version, sizeof(s_status.available_version));
    } else {
        s_status.available_version[0] = '\0';
    }
    status_unlock();

    if (err == ESP_OK) {
        store_available(available ? candidate.version : NULL);
        ESP_LOGI(TAG, "online firmware %s, installed firmware %s",
                 candidate.version, s_status.current_version);
        if (available) {
            ESP_LOGW(TAG, "firmware update %s is available", candidate.version);
        } else {
            ESP_LOGI(TAG, "firmware is up to date");
        }
    } else {
        ESP_LOGW(TAG, "update check failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void update_task(void *arg)
{
    esp_https_ota_handle_t handle = NULL;
    esp_app_desc_t candidate = {0};
    esp_err_t err = open_image(&handle, &candidate);
    int image_size = handle ? esp_https_ota_get_image_size(handle) : -1;

    while (err == ESP_OK) {
        err = esp_https_ota_perform(handle);
        int bytes_read = esp_https_ota_get_image_len_read(handle);
        if (image_size > 0 && bytes_read >= 0) {
            status_lock();
            s_status.progress_pct = bytes_read * 100 / image_size;
            status_unlock();
        }
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
    }

    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = esp_https_ota_finish(handle);
        handle = NULL;
    }
    if (handle) {
        esp_https_ota_abort(handle);
    }

    status_lock();
    s_status.updating = false;
    s_status.last_error = err;
    status_unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update installed, restarting into %s", candidate.version);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "status mutex");

    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(s_status.current_version, app->version, sizeof(s_status.current_version));
    s_status.configured = CONFIG_GEIGER_OTA_URL[0] != '\0';
    s_status.last_error = ESP_OK;
    load_available();

    if (s_status.update_available &&
        strcmp(s_status.available_version, s_status.current_version) == 0) {
        s_status.update_available = false;
        s_status.available_version[0] = '\0';
        store_available(NULL);
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "could not confirm running image: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t ota_check_on_connect(void)
{
    if (!s_status.configured) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    status_lock();
    if (s_status.checking || s_status.updating) {
        status_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_status.checking = true;
    s_status.last_error = ESP_OK;
    status_unlock();

    if (xTaskCreate(check_task, "ota_check", OTA_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        status_lock();
        s_status.checking = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        status_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_start_update(void)
{
    if (!s_status.configured) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    status_lock();
    if (s_status.checking || s_status.updating) {
        status_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_status.updating = true;
    s_status.progress_pct = 0;
    s_status.last_error = ESP_OK;
    status_unlock();

    if (xTaskCreate(update_task, "ota_update", OTA_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        status_lock();
        s_status.updating = false;
        s_status.last_error = ESP_ERR_NO_MEM;
        status_unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ota_get_status(ota_status_t *status)
{
    if (!status || !s_lock) {
        return;
    }
    status_lock();
    *status = s_status;
    status_unlock();
}
