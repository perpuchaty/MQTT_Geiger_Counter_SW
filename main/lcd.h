#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void lcd_draw_startup_screen(void);
esp_err_t lcd_start_main_screen(void);

#ifdef __cplusplus
}
#endif