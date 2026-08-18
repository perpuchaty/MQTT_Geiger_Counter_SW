#include "geiger.h"

#include "config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "geiger";

static uint16_t          s_window[GEIGER_WINDOW_SEC];
static uint8_t           s_idx;
static uint8_t           s_filled;
static uint32_t          s_last_total;
static volatile uint32_t s_cpm;

static void geiger_task(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    s_last_total = board_tube_pulses();

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000));

        uint32_t total = board_tube_pulses();
        uint32_t delta = total - s_last_total;
        s_last_total = total;

        s_window[s_idx] = delta > UINT16_MAX ? UINT16_MAX : (uint16_t)delta;
        s_idx = (s_idx + 1) % GEIGER_WINDOW_SEC;
        if (s_filled < GEIGER_WINDOW_SEC) {
            s_filled++;
        }

        uint32_t sum = 0;
        for (int i = 0; i < s_filled; i++) {
            sum += s_window[i];
        }
        /* Scale a partially filled window up to a full minute. */
        s_cpm = sum * GEIGER_WINDOW_SEC / s_filled;
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
    return s_cpm / GEIGER_CPM_PER_USVH;
}

uint32_t geiger_total(void)
{
    return board_tube_pulses();
}
