#pragma once

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

void lcd_draw_startup_screen(void);
void lcd_draw_battery_animation(uint8_t frame);
esp_err_t lcd_backlight_init(void);
void lcd_set_backlight(uint8_t brightness);
bool lcd_handle_button(board_input_t input, bool pressed);
void lcd_request_shutdown_confirmation(void);
void lcd_fade_out_and_clear(void);
bool lcd_lamp_test_active(void);
esp_err_t lcd_start_main_screen(void);

#ifdef __cplusplus
}
#endif