#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_STORE_FILE_PATH "/spiffs/history.csv"

typedef struct {
    uint32_t ts;
    uint16_t cpm;
} history_point_t;

esp_err_t history_store_init(void);
esp_err_t history_store_append(uint32_t ts, uint16_t cpm);
esp_err_t history_store_query(uint32_t since_ts,
                              history_point_t *out,
                              size_t max,
                              size_t *out_count,
                              uint32_t *oldest_ts,
                              uint32_t *newest_ts);

#ifdef __cplusplus
}
#endif
