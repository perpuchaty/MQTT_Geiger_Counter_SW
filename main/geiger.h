#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sliding window used to derive counts per minute. */
#define GEIGER_WINDOW_SEC       60

/** Tube sensitivity: counts per minute that correspond to 1 uSv/h (SBM-20). */
#define GEIGER_CPM_PER_USVH     153.8f

/** Starts the background task that turns tube pulses into a rate. */
esp_err_t geiger_start(void);

uint32_t geiger_cpm(void);
float geiger_usvh(void);
uint32_t geiger_total(void);

#ifdef __cplusplus
}
#endif
