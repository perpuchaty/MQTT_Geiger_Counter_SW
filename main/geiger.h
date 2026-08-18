#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest sliding window the ring buffer can hold, see settings.tube_window_s. */
#define GEIGER_WINDOW_MAX_SEC   TUBE_WINDOW_MAX_S

/** Starts the background task that turns tube pulses into a rate. */
esp_err_t geiger_start(void);

uint32_t geiger_cpm(void);
float geiger_usvh(void);
uint32_t geiger_total(void);

#ifdef __cplusplus
}
#endif
