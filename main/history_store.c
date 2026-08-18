#include "history_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "hist_store";

#define HISTORY_BASE_PATH "/spiffs"
#define HISTORY_TMP_FILE_PATH "/spiffs/history.tmp"

/* Keep history under ~92% of SPIFFS and compact down to ~75% when exceeded. */
#define HISTORY_MAX_PCT  92u
#define HISTORY_TRIM_PCT 75u

static bool s_ready;
static size_t s_spiffs_total;

static size_t history_file_size(void)
{
    FILE *f = fopen(HISTORY_STORE_FILE_PATH, "rb");
    if (!f) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (size_t)n : 0;
}

static esp_err_t history_compact_if_needed(void)
{
    if (s_spiffs_total == 0) {
        return ESP_OK;
    }

    const size_t max_bytes = (s_spiffs_total * HISTORY_MAX_PCT) / 100u;
    const size_t trim_to_bytes = (s_spiffs_total * HISTORY_TRIM_PCT) / 100u;
    const size_t cur = history_file_size();
    if (cur <= max_bytes || cur <= trim_to_bytes) {
        return ESP_OK;
    }

    size_t keep = trim_to_bytes;
    if (keep > cur) {
        keep = cur;
    }
    size_t cut = cur - keep;

    FILE *src = fopen(HISTORY_STORE_FILE_PATH, "rb");
    if (!src) {
        return ESP_FAIL;
    }
    FILE *dst = fopen(HISTORY_TMP_FILE_PATH, "wb");
    if (!dst) {
        fclose(src);
        return ESP_FAIL;
    }

    if (fseek(src, (long)cut, SEEK_SET) != 0) {
        fclose(dst);
        fclose(src);
        remove(HISTORY_TMP_FILE_PATH);
        return ESP_FAIL;
    }

    if (cut > 0) {
        int ch;
        while ((ch = fgetc(src)) != EOF) {
            if (ch == '\n') {
                break;
            }
        }
    }

    char buf[256];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, rd, dst) != rd) {
            fclose(dst);
            fclose(src);
            remove(HISTORY_TMP_FILE_PATH);
            return ESP_FAIL;
        }
    }

    fclose(dst);
    fclose(src);

    remove(HISTORY_STORE_FILE_PATH);
    if (rename(HISTORY_TMP_FILE_PATH, HISTORY_STORE_FILE_PATH) != 0) {
        remove(HISTORY_TMP_FILE_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "history compacted: %u -> %u bytes", (unsigned)cur,
             (unsigned)history_file_size());
    return ESP_OK;
}

esp_err_t history_store_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = HISTORY_BASE_PATH,
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "spiffs mount");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(NULL, &total, &used), TAG, "spiffs info");
    s_spiffs_total = total;
    ESP_LOGI(TAG, "SPIFFS mounted at %s, used %u / %u bytes", HISTORY_BASE_PATH,
             (unsigned)used, (unsigned)total);

    FILE *f = fopen(HISTORY_STORE_FILE_PATH, "a");
    if (!f) {
        return ESP_FAIL;
    }
    fclose(f);

    s_ready = true;
    return ESP_OK;
}

esp_err_t history_store_append(uint32_t ts, uint16_t cpm)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(HISTORY_STORE_FILE_PATH, "a");
    if (!f) {
        return ESP_FAIL;
    }
    fprintf(f, "%lu,%u\n", (unsigned long)ts, (unsigned)cpm);
    fclose(f);

    esp_err_t err = history_compact_if_needed();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "history compact skipped: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t history_store_query(uint32_t since_ts,
                              history_point_t *out,
                              size_t max,
                              size_t *out_count,
                              uint32_t *oldest_ts,
                              uint32_t *newest_ts)
{
    ESP_RETURN_ON_FALSE(out && max > 0 && out_count, ESP_ERR_INVALID_ARG, TAG, "bad arg");

    *out_count = 0;
    if (oldest_ts) {
        *oldest_ts = 0;
    }
    if (newest_ts) {
        *newest_ts = 0;
    }

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(HISTORY_STORE_FILE_PATH, "r");
    if (!f) {
        return ESP_OK;
    }

    size_t matched = 0;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        unsigned long ts_ul = 0;
        unsigned cpm_u = 0;
        if (sscanf(line, "%lu,%u", &ts_ul, &cpm_u) != 2) {
            continue;
        }

        uint32_t ts = (uint32_t)ts_ul;
        if (ts < since_ts) {
            continue;
        }

        if (oldest_ts && *oldest_ts == 0) {
            *oldest_ts = ts;
        }
        if (newest_ts) {
            *newest_ts = ts;
        }

        size_t slot = matched % max;
        out[slot].ts = ts;
        out[slot].cpm = cpm_u > UINT16_MAX ? UINT16_MAX : (uint16_t)cpm_u;
        matched++;
    }
    fclose(f);

    if (matched == 0) {
        return ESP_OK;
    }

    size_t keep = matched < max ? matched : max;
    if (matched > max) {
        size_t first = matched % max;
        history_point_t *tmp = malloc(sizeof(history_point_t) * keep);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < keep; i++) {
            tmp[i] = out[(first + i) % max];
        }
        memcpy(out, tmp, sizeof(history_point_t) * keep);
        free(tmp);
    }
    *out_count = keep;
    return ESP_OK;
}
