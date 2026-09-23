#pragma once

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

void lcd_draw_startup_screen(void);
typedef enum {
	LCD_BATTERY_IDLE,
	LCD_BATTERY_USB,
	LCD_BATTERY_FAULT,
	LCD_BATTERY_CHARGING,
} lcd_battery_state_t;

void lcd_draw_battery_status(lcd_battery_state_t state, uint8_t frame, int voltage_mv);
void lcd_clear(void);
esp_err_t lcd_backlight_init(void);
void lcd_set_backlight(uint8_t brightness);
void lcd_wake_backlight(void);
typedef enum {
	LCD_ACTION_NONE,
	LCD_ACTION_SHUTDOWN,
	LCD_ACTION_POWER_SAVE_ENABLE,
	LCD_ACTION_POWER_SAVE_DISABLE,
} lcd_action_t;

lcd_action_t lcd_handle_button(board_input_t input, bool pressed);
void lcd_request_shutdown_confirmation(void);
void lcd_request_power_save_confirmation(void);
void lcd_show_deep_discharge(void);
void lcd_fade_out_and_clear(void);
bool lcd_lamp_test_active(void);
bool lcd_menu_active(void);
esp_err_t lcd_start_main_screen(void);
void lcd_refresh(void);
void lcd_refresh_measurements(void);

#ifdef __cplusplus
}
#endif