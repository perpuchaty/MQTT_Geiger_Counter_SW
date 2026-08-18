#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest sliding window the ring buffer can hold, see settings.tube_window_s. */
#define GEIGER_WINDOW_MAX_SEC   TUBE_WINDOW_MAX_S

/** Trend history: one CPM sample every period, covering the last 2 hours in RAM. */
#define GEIGER_HISTORY_LEN      120
#define GEIGER_HISTORY_PERIOD_S 60

/** Starts the background task that turns tube pulses into a rate. */
esp_err_t geiger_start(void);

uint32_t geiger_cpm(void);
float geiger_usvh(void);
uint32_t geiger_total(void);

/** Copies up to max history samples, oldest first, and returns how many were written. */
size_t geiger_history(uint16_t *out, size_t max);

#ifdef __cplusplus
}
#endif
