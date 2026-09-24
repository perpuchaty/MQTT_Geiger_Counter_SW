#include "lcd.h"

#include <stdio.h>
#include <time.h>

#include "config.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geiger.h"
#include "mqtt.h"
#include "ota.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "lcd";
static volatile uint8_t s_backlight_target;
static volatile uint8_t s_backlight_current;
static volatile int64_t s_last_activity_us;
static volatile bool s_display_busy;
static TaskHandle_t s_display_task;
static uint8_t s_battery_animation_frame;

#define LCD_AUTO_DIM_DELAY_US  (30LL * 1000000LL)
#define LCD_MENU_ITEM_COUNT    9
#define LCD_SYSTEM_FIELD_COUNT 6

typedef enum {
    LCD_SCREEN_MAIN,
    LCD_SCREEN_CHART,
    LCD_SCREEN_MENU,
    LCD_SCREEN_WIFI,
    LCD_SCREEN_MQTT,
    LCD_SCREEN_SYSTEM,
    LCD_SCREEN_TUBE,
    LCD_SCREEN_TIME,
    LCD_SCREEN_HV,
    LCD_SCREEN_FIRMWARE,
    LCD_SCREEN_LAMP_TEST,
    LCD_SCREEN_POWER_SAVE,
    LCD_SCREEN_SHUTDOWN,
    LCD_SCREEN_DEEP_DISCHARGE,
    LCD_SCREEN_OFF,
} lcd_screen_t;

static volatile lcd_screen_t s_screen = LCD_SCREEN_MAIN;
static lcd_screen_t s_screen_before_shutdown = LCD_SCREEN_MAIN;
static lcd_screen_t s_screen_before_power_save = LCD_SCREEN_MAIN;
static uint8_t s_menu_item;
static bool s_wifi_forget_confirm;
static esp_err_t s_wifi_forget_error;
static uint8_t s_system_field;
static bool s_system_editing;
static settings_t s_system_settings;
static uint8_t s_tube_field;
static bool s_tube_editing;
static settings_t s_tube_settings;
static uint8_t s_time_field;
static bool s_time_editing;
static settings_t s_time_settings;
static uint8_t s_hv_field;
static bool s_hv_editing;
static bool s_firmware_install_confirm;
static esp_err_t s_firmware_action_error;
static volatile bool s_lamp_test_active;

static void backlight_fade_task(void *arg)
{
    uint8_t brightness = 0;

    for (;;) {
        uint8_t target = s_backlight_target;
        const settings_t *settings = settings_get();
        if (settings->lcd_auto_dim &&
            esp_timer_get_time() - s_last_activity_us >= LCD_AUTO_DIM_DELAY_US) {
            target = target == 0 ? 0 : (target < 10 ? 1 : target / 10);
        }
        if (brightness < target) {
            brightness++;
            board_backlight_set(brightness);
        } else if (brightness > target) {
            brightness--;
            board_backlight_set(brightness);
        }
        s_backlight_current = brightness;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t lcd_backlight_init(void)
{
    s_backlight_target = 0;
    s_last_activity_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(board_backlight_set(0), TAG, "backlight off");
    ESP_RETURN_ON_FALSE(xTaskCreate(backlight_fade_task, "backlight", 2048, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "backlight task");
    lcd_set_backlight(settings_get()->lcd_brightness);
    return ESP_OK;
}

void lcd_set_backlight(uint8_t brightness)
{
    s_backlight_target = brightness > 100 ? 100 : brightness;
}

void lcd_wake_backlight(void)
{
    s_last_activity_us = esp_timer_get_time();
    lcd_set_backlight(settings_get()->lcd_brightness);
}

static void draw_bluetooth_icon(u8g2_t *display, int x, int y, bool connected)
{
    u8g2_DrawLine(display, x + 3, y, x + 3, y + 10);
    u8g2_DrawLine(display, x + 3, y, x + 7, y + 3);
    u8g2_DrawLine(display, x + 7, y + 3, x + 3, y + 5);
    u8g2_DrawLine(display, x + 3, y + 5, x + 7, y + 8);
    u8g2_DrawLine(display, x + 7, y + 8, x + 3, y + 10);
    if (!connected) {
        u8g2_DrawLine(display, x, y + 10, x + 9, y);
    }
}

static void draw_wifi_icon(u8g2_t *display, int x, int y, bool connected)
{
    u8g2_DrawCircle(display, x + 8, y + 10, 8, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2_DrawCircle(display, x + 8, y + 10, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
    u8g2_DrawDisc(display, x + 8, y + 10, 1, U8G2_DRAW_ALL);
    if (!connected) {
        u8g2_DrawLine(display, x, y + 10, x + 16, y);
    }
}

static void draw_battery_icon(u8g2_t *display, int x, int y)
{
    const int width = 19;
    const int height = 10;
    bool chrg_level = board_input_level(BOARD_IN_CHRG);
    bool stby_level = board_input_level(BOARD_IN_STBY);
    bool charger_fault = chrg_level && stby_level;
    bool charging_active = !chrg_level && stby_level;

    u8g2_DrawFrame(display, x, y, width - 2, height);
    u8g2_DrawBox(display, x + width - 2, y + 3, 2, height - 6);
    if (charger_fault) {
        u8g2_DrawLine(display, x + 2, y + 2, x + width - 5, y + height - 3);
        u8g2_DrawLine(display, x + 2, y + height - 3, x + width - 5, y + 2);
        return;
    }

    if (settings_get()->batt_charge_en && charging_active) {
        uint8_t bars = (s_battery_animation_frame++ % 4) + 1;
        for (uint8_t bar = 0; bar < bars; bar++) {
            u8g2_DrawBox(display, x + 2 + bar * 4, y + 2, 3, height - 4);
        }
        return;
    }

    int voltage_mv;
    if (board_adc_get_mv(BOARD_ADC_VLATCH, &voltage_mv) != ESP_OK) {
        return;
    }
    uint8_t bars = (board_battery_percentage(voltage_mv) + 24) / 25;
    for (uint8_t bar = 0; bar < bars; bar++) {
        u8g2_DrawBox(display, x + 2 + bar * 4, y + 2, 3, height - 4);
    }
}

static void draw_main_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[16];
    time_t now;
    struct tm local_time;

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    if (!settings_get()->power_save_mode) {
        draw_bluetooth_icon(display, 1, 1, wifi_is_bluetooth_connected());
        draw_wifi_icon(display, 13, 1, wifi_is_connected());
    }
    if (board_hv_is_enabled()) {
        u8g2_DrawFrame(display, 31, 1, 17, 11);
        u8g2_SetFont(display, u8g2_font_5x7_tf);
        u8g2_DrawStr(display, 34, 9, "HV");
    }
    draw_battery_icon(display, LCD_WIDTH - 22, 1);

    now = time(NULL);
    if (now > 1609459200 && localtime_r(&now, &local_time) != NULL) {
        snprintf(text, sizeof(text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, text)) / 2, 10, text);
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);

    uint32_t dose_x100 = (uint32_t)(geiger_usvh() * 100.0f + 0.5f);
    snprintf(text, sizeof(text), "%lu.%02lu", (unsigned long)(dose_x100 / 100),
             (unsigned long)(dose_x100 % 100));
    u8g2_SetFont(display, u8g2_font_logisoso24_tf);
    u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, text)) / 2, 40, text);

    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, "uSv/h")) / 2, 49, "uSv/h");
    u8g2_DrawHLine(display, 0, 52, LCD_WIDTH);
    snprintf(text, sizeof(text), "RATE  %lu CPM", (unsigned long)geiger_cpm());
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, text)) / 2, 62, text);
    u8g2_SendBuffer(display);
}

static void draw_chart_screen(void)
{
    u8g2_t *display = board_lcd();
    uint16_t history[GEIGER_HISTORY_LEN];
    char text[24];

    if (display == NULL) {
        return;
    }

    size_t count = geiger_history(history, GEIGER_HISTORY_LEN);
    uint16_t maximum = 1;
    for (size_t i = 0; i < count; i++) {
        if (history[i] > maximum) {
            maximum = history[i];
        }
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 2, 7, "CPM TREND");
    snprintf(text, sizeof(text), "MAX %u", maximum);
    u8g2_DrawStr(display, LCD_WIDTH - u8g2_GetStrWidth(display, text) - 2, 7, text);
    u8g2_DrawHLine(display, 0, 10, LCD_WIDTH);

    const int chart_top = 12;
    const int chart_bottom = 49;
    const int chart_height = chart_bottom - chart_top;
    if (count < 2) {
        u8g2_DrawStr(display, 34, 33, "COLLECTING...");
    } else {
        for (size_t i = 1; i < count; i++) {
            int x0 = (int)((i - 1) * (LCD_WIDTH - 1) / (count - 1));
            int x1 = (int)(i * (LCD_WIDTH - 1) / (count - 1));
            int y0 = chart_bottom - (int)((uint32_t)history[i - 1] * chart_height / maximum);
            int y1 = chart_bottom - (int)((uint32_t)history[i] * chart_height / maximum);
            u8g2_DrawLine(display, x0, y0, x1, y1);
        }
    }

    u8g2_DrawHLine(display, 0, 51, LCD_WIDTH);
    uint32_t dose_x100 = (uint32_t)(geiger_usvh() * 100.0f + 0.5f);
    snprintf(text, sizeof(text), "NOW %lu.%02lu uSv/h",
             (unsigned long)(dose_x100 / 100), (unsigned long)(dose_x100 % 100));
    u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, text)) / 2, 62, text);
    u8g2_SendBuffer(display);
}

static void draw_menu_screen(void)
{
    u8g2_t *display = board_lcd();
    const char *items[] = { "Main screen", "Wi-Fi settings", "MQTT status", "System settings",
                            "Tube settings", "Time settings", "High voltage", "Firmware Update",
                            "Lamp test" };
    const size_t item_count = sizeof(items) / sizeof(items[0]);
    const uint8_t first_item = s_menu_item < 2 ? 0 : s_menu_item - 1;
    const int item_y[] = { 29, 49 };

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "MENU");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    for (size_t row = 0; row < 2; row++) {
        size_t item = first_item + row;
        if (item < item_count) {
            u8g2_DrawStr(display, 9, item_y[row], items[item]);
        }
    }
    u8g2_DrawFrame(display, 3, 15 + (s_menu_item - first_item) * 20, LCD_WIDTH - 13, 18);

    const int scrollbar_y = 16;
    const int scrollbar_h = 40;
    const int thumb_h = scrollbar_h * 2 / item_count;
    const int thumb_y = scrollbar_y + (scrollbar_h - thumb_h) * s_menu_item / (item_count - 1);
    u8g2_DrawFrame(display, LCD_WIDTH - 8, scrollbar_y, 5, scrollbar_h);
    u8g2_DrawBox(display, LCD_WIDTH - 7, thumb_y + 1, 3, thumb_h - 2);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_SendBuffer(display);
}

static void draw_firmware_screen(void)
{
    u8g2_t *display = board_lcd();
    ota_status_t status = {0};
    char text[32];

    if (display == NULL) {
        return;
    }

    ota_get_status(&status);
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "FIRMWARE UPDATE");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "INSTALLED: %.14s", status.current_version);
    u8g2_DrawStr(display, 4, 24, text);
    snprintf(text, sizeof(text), "ONLINE: %.17s",
             status.available_version[0] ? status.available_version : "UNKNOWN");
    u8g2_DrawStr(display, 4, 35, text);

    if (status.updating) {
        const int progress = status.progress_pct < 0 ? 0
                             : status.progress_pct > 100 ? 100
                             : status.progress_pct;
        const int bar_x = 4;
        const int bar_y = 50;
        const int bar_width = LCD_WIDTH - 8;
        const int bar_height = 11;
        const int fill_width = (bar_width - 4) * progress / 100;

        snprintf(text, sizeof(text), "DOWNLOADING  %d%%", progress);
        u8g2_DrawStr(display, (LCD_WIDTH - u8g2_GetStrWidth(display, text)) / 2, 46, text);
        u8g2_DrawFrame(display, bar_x, bar_y, bar_width, bar_height);
        if (fill_width > 0) {
            u8g2_DrawBox(display, bar_x + 2, bar_y + 2, fill_width, bar_height - 4);
        }
    } else if (status.checking) {
        u8g2_DrawStr(display, 4, 46, "CHECKING SERVER...");
        u8g2_DrawStr(display, 4, 58, "LEFT BACK");
    } else if (s_firmware_install_confirm) {
        u8g2_DrawStr(display, 4, 46, "INSTALL THIS VERSION?");
        u8g2_DrawStr(display, 4, 58, "ENTER YES  LEFT NO");
    } else {
        esp_err_t error = s_firmware_action_error != ESP_OK
                              ? s_firmware_action_error
                              : status.last_error;
        if (error != ESP_OK) {
            snprintf(text, sizeof(text), "ERROR: %.19s", esp_err_to_name(error));
        } else if (status.update_available) {
            snprintf(text, sizeof(text), "NEW VERSION AVAILABLE");
        } else {
            snprintf(text, sizeof(text), "ENTER INSTALL");
        }
        u8g2_DrawStr(display, 4, 46, text);
        u8g2_DrawStr(display, 4, 58, "LEFT BACK  RIGHT CHECK");
    }
    u8g2_SendBuffer(display);
}

static const char *sound_name(uint8_t sound)
{
    static const char *names[] = { "SHORT", "NORMAL", "LONG" };
    return sound < SOUND_TYPE_COUNT ? names[sound] : "NORMAL";
}

static void draw_system_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[24];
    const int row_y[] = { 24, 35, 46, 57 };
    const uint8_t first_field = s_system_field < 4 ? 0 : s_system_field - 3;

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "SYSTEM SETTINGS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    for (uint8_t row = 0; row < 4; row++) {
        uint8_t field = first_field + row;
        if (field == 0) {
            snprintf(text, sizeof(text), "SOUND:      %s", sound_name(s_system_settings.spk_sound));
        } else if (field == 1) {
            snprintf(text, sizeof(text), "VOLUME:     %u", s_system_settings.spk_volume);
        } else if (field == 2) {
            snprintf(text, sizeof(text), "BRIGHTNESS: %u %%", s_system_settings.lcd_brightness);
        } else if (field == 3) {
            snprintf(text, sizeof(text), "AUTO DIM:   %s", s_system_settings.lcd_auto_dim ? "ON" : "OFF");
        } else if (field == 4) {
            snprintf(text, sizeof(text), "POWER SAVE: %s",
                     s_system_settings.power_save_mode ? "ON" : "OFF");
        } else {
            snprintf(text, sizeof(text), "CHARGING:   %s",
                     s_system_settings.batt_charge_en ? "ON" : "OFF");
        }
        u8g2_DrawStr(display, 7, row_y[row], text);
    }
    u8g2_DrawFrame(display, 2, 15 + (s_system_field - first_field) * 11,
                   LCD_WIDTH - 12, 11);
    const int scrollbar_y = 16;
    const int scrollbar_h = 43;
    const int thumb_h = scrollbar_h * 4 / LCD_SYSTEM_FIELD_COUNT;
    const int thumb_y = scrollbar_y +
                        (scrollbar_h - thumb_h) * s_system_field /
                            (LCD_SYSTEM_FIELD_COUNT - 1);
    u8g2_DrawFrame(display, LCD_WIDTH - 8, scrollbar_y, 5, scrollbar_h);
    u8g2_DrawBox(display, LCD_WIDTH - 7, thumb_y + 1, 3, thumb_h - 2);
    if (s_system_editing) {
        u8g2_DrawBox(display, 120, 4, 4, 4);
    }
    u8g2_SendBuffer(display);
}

static void draw_mqtt_screen(void)
{
    u8g2_t *display = board_lcd();
    char broker[24];
    char text[32];
    const settings_t *settings = settings_get();

    if (display == NULL) {
        return;
    }

    strlcpy(broker, settings->mqtt_uri, sizeof(broker));
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "MQTT STATUS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "BROKER: %.23s", broker[0] ? broker : "NOT SET");
    u8g2_DrawStr(display, 4, 25, text);
    snprintf(text, sizeof(text), "STATE: %s", mqtt_state_str());
    u8g2_DrawStr(display, 4, 37, text);
    u8g2_DrawStr(display, 4, 49, settings->mqtt_enabled ? "PUBLISHING: ENABLED" : "PUBLISHING: OFF");
    u8g2_DrawStr(display, 4, 61, "LEFT BACK");
    u8g2_SendBuffer(display);
}

static void draw_wifi_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[32];
    char ip[16];

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "WI-FI SETTINGS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);

    if (s_wifi_forget_confirm) {
        u8g2_DrawStr(display, 4, 27, "FORGET SAVED NETWORK?");
        u8g2_DrawStr(display, 4, 42, "ENTER YES");
        u8g2_DrawStr(display, 4, 61, "LEFT CANCEL");
        u8g2_SendBuffer(display);
        return;
    }

    snprintf(text, sizeof(text), "STATUS: %s", wifi_is_connected() ? "CONNECTED" : "DISCONNECTED");
    u8g2_DrawStr(display, 4, 26, text);
    wifi_get_ip_str(ip, sizeof(ip));
    snprintf(text, sizeof(text), "IP: %s", ip);
    u8g2_DrawStr(display, 4, 38, text);
    if (s_wifi_forget_error != ESP_OK) {
        char error[28];
        snprintf(error, sizeof(error), "ERROR: %.20s", esp_err_to_name(s_wifi_forget_error));
        u8g2_DrawStr(display, 4, 50, error);
    } else if (wifi_has_credentials()) {
        u8g2_DrawStr(display, 4, 50, "ENTER FORGET NETWORK");
    } else if (wifi_prov_running() & WIFI_PROV_SOFTAP) {
        snprintf(text, sizeof(text), "AP: %.25s", wifi_softap_ssid());
        u8g2_DrawStr(display, 4, 50, text);
    } else {
        u8g2_DrawStr(display, 4, 50, "NO SAVED NETWORK");
    }
    u8g2_DrawStr(display, 4, 61, "LEFT BACK");
    u8g2_SendBuffer(display);
}

static void draw_tube_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[32];
    const int row_y[] = { 26, 38, 50 };
    uint32_t sensitivity_x10 = (uint32_t)(s_tube_settings.tube_cpm_per_usvh * 10.0f + 0.5f);

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "TUBE SETTINGS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "VOLTAGE: %u V", s_tube_settings.tube_target_v);
    u8g2_DrawStr(display, 7, row_y[0], text);
    snprintf(text, sizeof(text), "WINDOW:  %u s", s_tube_settings.tube_window_s);
    u8g2_DrawStr(display, 7, row_y[1], text);
    snprintf(text, sizeof(text), "SENS:    %lu.%01lu CPM/uSv", (unsigned long)(sensitivity_x10 / 10),
             (unsigned long)(sensitivity_x10 % 10));
    u8g2_DrawStr(display, 7, row_y[2], text);
    u8g2_DrawFrame(display, 2, 15 + s_tube_field * 12, LCD_WIDTH - 4, 12);
    u8g2_DrawStr(display, 4, 61, s_tube_editing ? "L/R CHANGE  ENTER SAVE" : "L/R SELECT  ENTER EDIT");
    u8g2_SendBuffer(display);
}

static void draw_time_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[28];
    int offset_min = s_time_settings.timezone_offset_min;
    int absolute_min = offset_min < 0 ? -offset_min : offset_min;
    const int row_y[] = { 29, 45 };

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "TIME SETTINGS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "UTC OFFSET: %c%02d:%02d", offset_min < 0 ? '-' : '+',
             absolute_min / 60, absolute_min % 60);
    u8g2_DrawStr(display, 7, row_y[0], text);
    snprintf(text, sizeof(text), "SUMMER TIME: %s", s_time_settings.daylight_saving ? "ON" : "OFF");
    u8g2_DrawStr(display, 7, row_y[1], text);
    u8g2_DrawFrame(display, 2, 17 + s_time_field * 16, LCD_WIDTH - 4, 14);
    u8g2_DrawStr(display, 4, 61, s_time_editing ? "L/R CHANGE  ENTER SAVE" : "L/R SELECT  ENTER EDIT");
    u8g2_SendBuffer(display);
}

static void draw_hv_screen(void)
{
    u8g2_t *display = board_lcd();
    char text[24];
    int adc_raw = 0;
    int voltage_mv = 0;
    const int row_y[] = { 22, 31, 40, 49, 58 };

    if (display == NULL) {
        return;
    }

    board_adc_get_raw(BOARD_ADC_TUBE, &adc_raw);
    board_tube_voltage_get_mv(&voltage_mv);
    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "HIGH VOLTAGE");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "ENABLE: %s", board_hv_is_enabled() ? "ON" : "OFF");
    u8g2_DrawStr(display, 7, row_y[0], text);
    uint32_t duty_x10 = (uint32_t)(board_hv_output_duty_pct() * 10.0f + 0.5f);
    snprintf(text, sizeof(text), "DUTY:   %lu.%lu %%", (unsigned long)(duty_x10 / 10),
             (unsigned long)(duty_x10 % 10));
    u8g2_DrawStr(display, 7, row_y[1], text);
    snprintf(text, sizeof(text), "FREQ:   %lu Hz", (unsigned long)board_hv_freq_hz());
    u8g2_DrawStr(display, 7, row_y[2], text);
    snprintf(text, sizeof(text), "ADC:    %d raw", adc_raw);
    u8g2_DrawStr(display, 7, row_y[3], text);
    snprintf(text, sizeof(text), "TUBE:   %d V", (voltage_mv + 500) / 1000);
    u8g2_DrawStr(display, 7, row_y[4], text);
    u8g2_DrawFrame(display, 2, 15 + s_hv_field * 9, LCD_WIDTH - 4, 9);
    if (s_hv_editing) {
        u8g2_DrawBox(display, 120, 4, 4, 4);
    }
    u8g2_SendBuffer(display);
}

static void draw_lamp_test_screen(void)
{
    u8g2_t *display = board_lcd();
    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    if (s_lamp_test_active) {
        u8g2_DrawBox(display, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    } else {
        u8g2_SetFont(display, u8g2_font_6x10_tf);
        u8g2_DrawStr(display, 4, 10, "LAMP TEST");
        u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
        u8g2_DrawStr(display, 22, 35, "HOLD ENTER");
        u8g2_SetFont(display, u8g2_font_5x7_tf);
        u8g2_DrawStr(display, 4, 61, "LEFT BACK");
    }
    u8g2_SendBuffer(display);
}

static void draw_shutdown_screen(void)
{
    u8g2_t *display = board_lcd();
    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "SHUT DOWN?");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_DrawStr(display, 24, 35, "ENTER  YES");
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 4, 61, "LEFT CANCEL");
    u8g2_SendBuffer(display);
}

static void draw_power_save_screen(void)
{
    u8g2_t *display = board_lcd();
    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "POWER SAVE?");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_DrawStr(display, 24, 32,
                 settings_get()->power_save_mode ? "DISABLE" : "ENABLE");
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 4, 48, "ENTER  YES");
    u8g2_DrawStr(display, 4, 61, "LEFT   CANCEL");
    u8g2_SendBuffer(display);
}

static void draw_deep_discharge_screen(void)
{
    u8g2_t *display = board_lcd();
    const int battery_x = 27;
    const int battery_y = 20;
    const int battery_w = 70;
    const int battery_h = 32;

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 25, 10, "BATTERY EMPTY");
    u8g2_DrawFrame(display, battery_x, battery_y, battery_w, battery_h);
    u8g2_DrawBox(display, battery_x + battery_w, battery_y + 9, 5, battery_h - 18);
    u8g2_DrawLine(display, battery_x + 8, battery_y + 7,
                 battery_x + battery_w - 8, battery_y + battery_h - 7);
    u8g2_DrawLine(display, battery_x + battery_w - 8, battery_y + 7,
                 battery_x + 8, battery_y + battery_h - 7);
    u8g2_SendBuffer(display);
}

static void time_adjust(int direction)
{
    if (s_time_field == 0) {
        int offset_min = s_time_settings.timezone_offset_min + direction * 30;
        s_time_settings.timezone_offset_min = offset_min < TIMEZONE_OFFSET_MIN ? TIMEZONE_OFFSET_MIN :
                                                offset_min > TIMEZONE_OFFSET_MAX ? TIMEZONE_OFFSET_MAX : offset_min;
    } else {
        s_time_settings.daylight_saving = !s_time_settings.daylight_saving;
    }
}

static void hv_adjust(int direction)
{
    if (s_hv_field == 1) {
        board_hv_set_duty(board_hv_duty_pct() + direction);
    } else if (s_hv_field == 2) {
        int frequency = (int)board_hv_freq_hz() + direction * 100;
        board_hv_set_freq(frequency < 100 ? 100 : (uint32_t)frequency);
    }
}

static void tube_adjust(int direction)
{
    if (s_tube_field == 0) {
        int voltage = s_tube_settings.tube_target_v + direction * 5;
        s_tube_settings.tube_target_v = voltage < TUBE_TARGET_MIN_V ? TUBE_TARGET_MIN_V :
                                          voltage > TUBE_TARGET_MAX_V ? TUBE_TARGET_MAX_V : voltage;
    } else if (s_tube_field == 1) {
        int window = s_tube_settings.tube_window_s + direction * 5;
        s_tube_settings.tube_window_s = window < TUBE_WINDOW_MIN_S ? TUBE_WINDOW_MIN_S :
                                         window > TUBE_WINDOW_MAX_S ? TUBE_WINDOW_MAX_S : window;
    } else {
        s_tube_settings.tube_cpm_per_usvh += direction * 0.1f;
        if (s_tube_settings.tube_cpm_per_usvh < TUBE_CPM_MIN) {
            s_tube_settings.tube_cpm_per_usvh = TUBE_CPM_MIN;
        } else if (s_tube_settings.tube_cpm_per_usvh > TUBE_CPM_MAX) {
            s_tube_settings.tube_cpm_per_usvh = TUBE_CPM_MAX;
        }
    }
}

static void system_adjust(int direction)
{
    if (s_system_field == 0) {
        int sound = s_system_settings.spk_sound + direction;
        s_system_settings.spk_sound = sound < 0 ? SOUND_TYPE_COUNT - 1 :
                                              sound >= SOUND_TYPE_COUNT ? SOUND_SHORT : sound;
    } else if (s_system_field == 1) {
        int volume = s_system_settings.spk_volume + direction * 10;
        s_system_settings.spk_volume = volume < 0 ? 0 : volume > 200 ? 200 : volume;
    } else if (s_system_field == 2) {
        int brightness = s_system_settings.lcd_brightness + direction * 5;
        s_system_settings.lcd_brightness = brightness < 0 ? 0 : brightness > 100 ? 100 : brightness;
        lcd_set_backlight(s_system_settings.lcd_brightness);
    } else if (s_system_field == 3) {
        s_system_settings.lcd_auto_dim = !s_system_settings.lcd_auto_dim;
    } else if (s_system_field == 4) {
        s_system_settings.power_save_mode = !s_system_settings.power_save_mode;
    } else {
        s_system_settings.batt_charge_en = !s_system_settings.batt_charge_en;
    }
}

lcd_action_t lcd_handle_button(board_input_t input, bool pressed)
{
    if (s_screen == LCD_SCREEN_SHUTDOWN) {
        if (!pressed) {
            return LCD_ACTION_NONE;
        }
        s_last_activity_us = esp_timer_get_time();
        if (input == BOARD_IN_BTN_ENTER) {
            return LCD_ACTION_SHUTDOWN;
        }
        if (input == BOARD_IN_BTN_LEFT) {
            s_screen = s_screen_before_shutdown;
        }
        return LCD_ACTION_NONE;
    }
    if (s_screen == LCD_SCREEN_POWER_SAVE) {
        if (!pressed) {
            return LCD_ACTION_NONE;
        }
        s_last_activity_us = esp_timer_get_time();
        if (input == BOARD_IN_BTN_ENTER) {
            bool enable = !settings_get()->power_save_mode;
            s_screen = s_screen_before_power_save;
            return enable ? LCD_ACTION_POWER_SAVE_ENABLE : LCD_ACTION_POWER_SAVE_DISABLE;
        }
        if (input == BOARD_IN_BTN_LEFT) {
            s_screen = s_screen_before_power_save;
        }
        return LCD_ACTION_NONE;
    }
    if (s_screen == LCD_SCREEN_LAMP_TEST && input == BOARD_IN_BTN_ENTER) {
        s_lamp_test_active = pressed;
        return LCD_ACTION_NONE;
    }
    if (!pressed) {
        return LCD_ACTION_NONE;
    }
    s_last_activity_us = esp_timer_get_time();

    switch (s_screen) {
    case LCD_SCREEN_MAIN:
        if (input == BOARD_IN_BTN_ENTER) {
            s_screen = LCD_SCREEN_MENU;
        } else if (input == BOARD_IN_BTN_LEFT || input == BOARD_IN_BTN_RIGHT) {
            s_screen = LCD_SCREEN_CHART;
        }
        break;
    case LCD_SCREEN_CHART:
        if (input == BOARD_IN_BTN_ENTER) {
            s_screen = LCD_SCREEN_MENU;
        } else if (input == BOARD_IN_BTN_LEFT || input == BOARD_IN_BTN_RIGHT) {
            s_screen = LCD_SCREEN_MAIN;
        }
        break;
    case LCD_SCREEN_MENU:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_menu_item == 0) {
                s_screen = LCD_SCREEN_MAIN;
            } else if (s_menu_item == 1) {
                s_wifi_forget_confirm = false;
                s_wifi_forget_error = ESP_OK;
                s_screen = LCD_SCREEN_WIFI;
            } else if (s_menu_item == 2) {
                s_screen = LCD_SCREEN_MQTT;
            } else if (s_menu_item == 3) {
                s_system_settings = *settings_get();
                s_system_field = 0;
                s_system_editing = false;
                s_screen = LCD_SCREEN_SYSTEM;
            } else if (s_menu_item == 4) {
                s_tube_settings = *settings_get();
                s_tube_field = 0;
                s_tube_editing = false;
                s_screen = LCD_SCREEN_TUBE;
            } else if (s_menu_item == 5) {
                s_time_settings = *settings_get();
                s_time_field = 0;
                s_time_editing = false;
                s_screen = LCD_SCREEN_TIME;
            } else if (s_menu_item == 6) {
                s_hv_field = 0;
                s_hv_editing = false;
                s_screen = LCD_SCREEN_HV;
            } else if (s_menu_item == 7) {
                s_firmware_install_confirm = false;
                s_firmware_action_error = ESP_OK;
                s_screen = LCD_SCREEN_FIRMWARE;
            } else {
                s_lamp_test_active = true;
                s_screen = LCD_SCREEN_LAMP_TEST;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            s_menu_item = s_menu_item == 0 ? LCD_MENU_ITEM_COUNT - 1 : s_menu_item - 1;
        } else if (input == BOARD_IN_BTN_RIGHT) {
            s_menu_item = s_menu_item == LCD_MENU_ITEM_COUNT - 1 ? 0 : s_menu_item + 1;
        }
        break;
    case LCD_SCREEN_WIFI:
        if (input == BOARD_IN_BTN_ENTER && wifi_has_credentials()) {
            if (!s_wifi_forget_confirm) {
                s_wifi_forget_confirm = true;
            } else {
                esp_err_t err = wifi_forget();
                if (err == ESP_OK && wifi_radio_is_enabled()) {
                    err = wifi_prov_start(WIFI_PROV_SOFTAP);
                }
                s_wifi_forget_confirm = false;
                s_wifi_forget_error = err;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            if (s_wifi_forget_confirm) {
                s_wifi_forget_confirm = false;
            } else {
                s_screen = LCD_SCREEN_MENU;
            }
        }
        break;
    case LCD_SCREEN_MQTT:
        if (input == BOARD_IN_BTN_LEFT) {
            s_screen = LCD_SCREEN_MENU;
        }
        break;
    case LCD_SCREEN_SYSTEM:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_system_editing) {
                if (s_system_field == 4 &&
                    s_system_settings.power_save_mode != settings_get()->power_save_mode) {
                    s_system_editing = false;
                    return s_system_settings.power_save_mode ? LCD_ACTION_POWER_SAVE_ENABLE
                                                             : LCD_ACTION_POWER_SAVE_DISABLE;
                }
                if (settings_save(&s_system_settings) == ESP_OK) {
                    lcd_set_backlight(settings_get()->lcd_brightness);
                    board_apply_settings();
                }
                s_system_editing = false;
            } else {
                s_system_editing = true;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            if (s_system_editing) {
                system_adjust(-1);
            } else if (s_system_field == 0) {
                s_screen = LCD_SCREEN_MENU;
            } else {
                s_system_field--;
            }
        } else if (input == BOARD_IN_BTN_RIGHT) {
            if (s_system_editing) {
                system_adjust(1);
            } else if (s_system_field < LCD_SYSTEM_FIELD_COUNT - 1) {
                s_system_field++;
            }
        }
        break;
    case LCD_SCREEN_TUBE:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_tube_editing) {
                settings_save(&s_tube_settings);
                s_tube_editing = false;
            } else {
                s_tube_editing = true;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            if (s_tube_editing) {
                tube_adjust(-1);
            } else if (s_tube_field == 0) {
                s_screen = LCD_SCREEN_MENU;
            } else {
                s_tube_field--;
            }
        } else if (input == BOARD_IN_BTN_RIGHT) {
            if (s_tube_editing) {
                tube_adjust(1);
            } else if (s_tube_field < 2) {
                s_tube_field++;
            }
        }
        break;
    case LCD_SCREEN_TIME:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_time_editing) {
                settings_save(&s_time_settings);
                s_time_editing = false;
            } else {
                s_time_editing = true;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            if (s_time_editing) {
                time_adjust(-1);
            } else if (s_time_field == 0) {
                s_screen = LCD_SCREEN_MENU;
            } else {
                s_time_field--;
            }
        } else if (input == BOARD_IN_BTN_RIGHT) {
            if (s_time_editing) {
                time_adjust(1);
            } else if (s_time_field < 1) {
                s_time_field++;
            }
        }
        break;
    case LCD_SCREEN_HV:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_hv_field == 0) {
                bool previous = board_hv_is_enabled();
                bool enabled = !previous;
                esp_err_t err = board_hv_set_enabled(enabled);
                if (err == ESP_OK) {
                    settings_t updated = *settings_get();
                    updated.hv_start_enabled = enabled;
                    err = settings_save(&updated);
                    if (err != ESP_OK) {
                        board_hv_set_enabled(previous);
                    }
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save HV state: %s", esp_err_to_name(err));
                }
            } else {
                s_hv_editing = !s_hv_editing;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            if (s_hv_editing) {
                hv_adjust(-1);
            } else if (s_hv_field == 0) {
                s_screen = LCD_SCREEN_MENU;
            } else {
                s_hv_field--;
            }
        } else if (input == BOARD_IN_BTN_RIGHT) {
            if (s_hv_editing) {
                hv_adjust(1);
            } else if (s_hv_field < 2) {
                s_hv_field++;
            }
        }
        break;
    case LCD_SCREEN_FIRMWARE: {
        ota_status_t status = {0};
        ota_get_status(&status);
        if (input == BOARD_IN_BTN_ENTER && !status.checking && !status.updating) {
            if (s_firmware_install_confirm) {
                s_firmware_action_error = ota_start_update();
                if (s_firmware_action_error == ESP_OK) {
                    s_firmware_install_confirm = false;
                }
            } else {
                s_firmware_install_confirm = true;
            }
        } else if (input == BOARD_IN_BTN_LEFT && !status.updating) {
            if (s_firmware_install_confirm) {
                s_firmware_install_confirm = false;
            } else {
                s_screen = LCD_SCREEN_MENU;
            }
        } else if (input == BOARD_IN_BTN_RIGHT && !status.checking && !status.updating) {
            s_firmware_install_confirm = false;
            s_firmware_action_error = ota_check_on_connect();
        }
        break;
    }
    case LCD_SCREEN_LAMP_TEST:
        if (input == BOARD_IN_BTN_LEFT) {
            s_lamp_test_active = false;
            s_screen = LCD_SCREEN_MENU;
        }
        break;
    case LCD_SCREEN_POWER_SAVE:
    case LCD_SCREEN_SHUTDOWN:
    case LCD_SCREEN_DEEP_DISCHARGE:
    case LCD_SCREEN_OFF:
        break;
    }
    return LCD_ACTION_NONE;
}

void lcd_request_shutdown_confirmation(void)
{
    if (s_screen != LCD_SCREEN_SHUTDOWN && s_screen != LCD_SCREEN_OFF) {
        s_screen_before_shutdown = s_screen;
        s_lamp_test_active = false;
        s_last_activity_us = esp_timer_get_time();
        s_screen = LCD_SCREEN_SHUTDOWN;
        lcd_refresh();
    }
}

void lcd_request_power_save_confirmation(void)
{
    if (s_screen != LCD_SCREEN_SHUTDOWN && s_screen != LCD_SCREEN_POWER_SAVE &&
        s_screen != LCD_SCREEN_OFF) {
        s_screen_before_power_save = s_screen;
        s_lamp_test_active = false;
        s_last_activity_us = esp_timer_get_time();
        s_screen = LCD_SCREEN_POWER_SAVE;
        lcd_refresh();
    }
}

void lcd_fade_out_and_clear(void)
{
    s_screen = LCD_SCREEN_OFF;
    s_backlight_target = 0;
    while (s_backlight_current > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    while (s_display_busy) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    u8g2_t *display = board_lcd();
    if (display != NULL) {
        u8g2_ClearBuffer(display);
        u8g2_SendBuffer(display);
        u8g2_SetPowerSave(display, 1);
    }
}

bool lcd_lamp_test_active(void)
{
    return s_lamp_test_active;
}

bool lcd_menu_active(void)
{
    return s_screen >= LCD_SCREEN_MENU && s_screen <= LCD_SCREEN_LAMP_TEST;
}

void lcd_refresh(void)
{
    if (s_display_task != NULL) {
        xTaskNotifyGive(s_display_task);
    }
}

void lcd_refresh_measurements(void)
{
    if (s_screen == LCD_SCREEN_MAIN || s_screen == LCD_SCREEN_CHART) {
        lcd_refresh();
    }
}

static void main_screen_task(void *arg)
{
    for (;;) {
        s_display_busy = true;
        switch (s_screen) {
        case LCD_SCREEN_MAIN:
            draw_main_screen();
            break;
        case LCD_SCREEN_CHART:
            draw_chart_screen();
            break;
        case LCD_SCREEN_MENU:
            draw_menu_screen();
            break;
        case LCD_SCREEN_WIFI:
            draw_wifi_screen();
            break;
        case LCD_SCREEN_MQTT:
            draw_mqtt_screen();
            break;
        case LCD_SCREEN_SYSTEM:
            draw_system_screen();
            break;
        case LCD_SCREEN_TUBE:
            draw_tube_screen();
            break;
        case LCD_SCREEN_TIME:
            draw_time_screen();
            break;
        case LCD_SCREEN_HV:
            draw_hv_screen();
            break;
        case LCD_SCREEN_FIRMWARE:
            draw_firmware_screen();
            break;
        case LCD_SCREEN_LAMP_TEST:
            draw_lamp_test_screen();
            break;
        case LCD_SCREEN_POWER_SAVE:
            draw_power_save_screen();
            break;
        case LCD_SCREEN_SHUTDOWN:
            draw_shutdown_screen();
            break;
        case LCD_SCREEN_DEEP_DISCHARGE:
            draw_deep_discharge_screen();
            break;
        case LCD_SCREEN_OFF:
            break;
        }
        s_display_busy = false;
        TickType_t wait = s_screen == LCD_SCREEN_FIRMWARE ? pdMS_TO_TICKS(250) : portMAX_DELAY;
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

esp_err_t lcd_start_main_screen(void)
{
    ESP_RETURN_ON_FALSE(xTaskCreate(main_screen_task, "display", 3072, NULL, 4,
                                    &s_display_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "display task");
    return ESP_OK;
}

void lcd_draw_startup_screen(void)
{
    u8g2_t *display = board_lcd();

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 33, 10, "GEIGER COUNTER");
    u8g2_DrawHLine(display, 16, 14, LCD_WIDTH - 32);

    const int icon_x = LCD_WIDTH / 2;
    const int icon_y = 33;
    u8g2_DrawCircle(display, icon_x, icon_y, 16, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x, icon_y - 9, 5, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x - 8, icon_y + 5, 5, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x + 8, icon_y + 5, 5, U8G2_DRAW_ALL);
    u8g2_SetDrawColor(display, 0);
    u8g2_DrawDisc(display, icon_x, icon_y, 4, U8G2_DRAW_ALL);
    u8g2_SetDrawColor(display, 1);
    u8g2_DrawDisc(display, icon_x, icon_y, 2, U8G2_DRAW_ALL);

    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 34, 61, "INITIALIZING");
    u8g2_SendBuffer(display);
}

void lcd_show_deep_discharge(void)
{
    s_screen = LCD_SCREEN_DEEP_DISCHARGE;
    s_lamp_test_active = false;
    lcd_set_backlight(0);

    if (s_display_task != NULL) {
        lcd_refresh();
    } else {
        draw_deep_discharge_screen();
    }
}

void lcd_clear(void)
{
    u8g2_t *display = board_lcd();

    if (display != NULL) {
        u8g2_ClearBuffer(display);
        u8g2_SendBuffer(display);
    }
}

void lcd_draw_battery_status(lcd_battery_state_t state, uint8_t frame, int voltage_mv)
{
    u8g2_t *display = board_lcd();

    if (display == NULL) {
        return;
    }

    const int battery_x = 35;
    const int battery_y = 17;
    const int battery_w = 56;
    const int battery_h = 28;
    int segment_count = 0;

    if (state == LCD_BATTERY_CHARGING) {
        segment_count = (frame % 5) + 1;
    } else if (state == LCD_BATTERY_IDLE) {
        segment_count = (board_battery_percentage(voltage_mv) + 19) / 20;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    if (state == LCD_BATTERY_FAULT) {
        u8g2_DrawStr(display, 47, 10, "FAULT");
    } else if (state == LCD_BATTERY_CHARGING) {
        u8g2_DrawStr(display, 38, 10, "CHARGING");
    } else if (state == LCD_BATTERY_USB) {
        u8g2_DrawStr(display, 55, 10, "USB");
    } else {
        u8g2_DrawStr(display, 44, 10, "BATTERY");
    }
    u8g2_DrawFrame(display, battery_x, battery_y, battery_w, battery_h);
    u8g2_DrawBox(display, battery_x + battery_w, battery_y + 8, 4, battery_h - 16);
    for (int segment = 0; segment < segment_count; segment++) {
        u8g2_DrawBox(display, battery_x + 4 + segment * 10, battery_y + 4, 7, battery_h - 8);
    }
    if (state == LCD_BATTERY_FAULT) {
        u8g2_DrawLine(display, battery_x + 5, battery_y + 4,
                     battery_x + battery_w - 5, battery_y + battery_h - 4);
        u8g2_DrawLine(display, battery_x + battery_w - 5, battery_y + 4,
                     battery_x + 5, battery_y + battery_h - 4);
    } else if (state == LCD_BATTERY_USB) {
        u8g2_DrawBox(display, battery_x + 24, battery_y + 7, 8, 8);
        u8g2_DrawLine(display, battery_x + 28, battery_y + 15,
                     battery_x + 28, battery_y + 22);
        u8g2_DrawLine(display, battery_x + 24, battery_y + 22,
                     battery_x + 32, battery_y + 22);
    }
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 17, 61, "HOLD ENTER TO START");
    u8g2_SendBuffer(display);
}