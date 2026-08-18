#include "geiger.h"

#include "config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "geiger";

static uint16_t          s_window[GEIGER_WINDOW_MAX_SEC];
static uint16_t          s_idx;
static uint16_t          s_filled;
static uint16_t          s_window_len;
static uint32_t          s_last_total;
static volatile uint32_t s_cpm;

static void geiger_task(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    s_last_total = board_tube_pulses();
    s_window_len = settings_get()->tube_window_s;

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000));

        /* A window change from the web UI restarts the average. */
        uint16_t len = settings_get()->tube_window_s;
        if (len != s_window_len) {
            s_window_len = len;
            s_idx = 0;
            s_filled = 0;
        }

        uint32_t total = board_tube_pulses();
        uint32_t delta = total - s_last_total;
        s_last_total = total;

        s_window[s_idx] = delta > UINT16_MAX ? UINT16_MAX : (uint16_t)delta;
        s_idx = (s_idx + 1) % s_window_len;
        if (s_filled < s_window_len) {
            s_filled++;
        }

        uint32_t sum = 0;
        for (int i = 0; i < s_filled; i++) {
            sum += s_window[i];
        }
        /* Scale the window, however long and however full, up to a full minute. */
        s_cpm = sum * 60 / s_filled;
    }
}

esp_err_t geiger_start(void)
{
    ESP_RETURN_ON_FALSE(xTaskCreate(geiger_task, "geiger", 2560, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

uint32_t geiger_cpm(void)
{
    return s_cpm;
}

float geiger_usvh(void)
{
    return s_cpm / settings_get()->tube_cpm_per_usvh;
}

uint32_t geiger_total(void)
{
    return board_tube_pulses();
}
