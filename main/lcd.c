#include "lcd.h"

#include <stdio.h>
#include <time.h>

#include "config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geiger.h"
#include "mqtt.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "lcd";
static volatile uint8_t s_backlight_target;

typedef enum {
    LCD_SCREEN_MAIN,
    LCD_SCREEN_MENU,
    LCD_SCREEN_WIFI,
    LCD_SCREEN_MQTT,
    LCD_SCREEN_TUBE,
} lcd_screen_t;

static volatile lcd_screen_t s_screen = LCD_SCREEN_MAIN;
static uint8_t s_menu_item;
static uint8_t s_tube_field;
static bool s_tube_editing;
static settings_t s_tube_settings;

static void backlight_fade_task(void *arg)
{
    uint8_t brightness = 0;

    for (;;) {
        uint8_t target = s_backlight_target;
        if (brightness < target) {
            brightness++;
            board_backlight_set(brightness);
        } else if (brightness > target) {
            brightness--;
            board_backlight_set(brightness);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t lcd_backlight_init(void)
{
    s_backlight_target = 0;
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
    draw_bluetooth_icon(display, 3, 1, wifi_is_bluetooth_connected());
    draw_wifi_icon(display, 17, 1, wifi_is_connected());

    now = time(NULL);
    if (now > 1609459200 && localtime_r(&now, &local_time) != NULL) {
        snprintf(text, sizeof(text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, LCD_WIDTH - u8g2_GetStrWidth(display, text) - 3, 10, text);
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

static void draw_menu_screen(void)
{
    u8g2_t *display = board_lcd();
    const char *items[] = { "Main screen", "Wi-Fi settings", "MQTT status", "Tube settings" };
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
    char text[24];
    char ip[16];

    if (display == NULL) {
        return;
    }

    u8g2_ClearBuffer(display);
    u8g2_SetFont(display, u8g2_font_6x10_tf);
    u8g2_DrawStr(display, 4, 10, "WI-FI SETTINGS");
    u8g2_DrawHLine(display, 0, 13, LCD_WIDTH);
    u8g2_SetFont(display, u8g2_font_5x7_tf);
    snprintf(text, sizeof(text), "STATUS: %s", wifi_is_connected() ? "CONNECTED" : "DISCONNECTED");
    u8g2_DrawStr(display, 4, 26, text);
    wifi_get_ip_str(ip, sizeof(ip));
    snprintf(text, sizeof(text), "IP: %s", ip);
    u8g2_DrawStr(display, 4, 38, text);
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

void lcd_handle_button(board_input_t input, bool pressed)
{
    if (!pressed) {
        return;
    }

    switch (s_screen) {
    case LCD_SCREEN_MAIN:
        if (input == BOARD_IN_BTN_ENTER) {
            s_screen = LCD_SCREEN_MENU;
        } else if (input == BOARD_IN_BTN_RIGHT) {
            board_hv_set_duty(board_hv_duty_pct() + 1.0f);
        }
        break;
    case LCD_SCREEN_MENU:
        if (input == BOARD_IN_BTN_ENTER) {
            if (s_menu_item == 0) {
                s_screen = LCD_SCREEN_MAIN;
            } else if (s_menu_item == 1) {
                s_screen = LCD_SCREEN_WIFI;
            } else if (s_menu_item == 2) {
                s_screen = LCD_SCREEN_MQTT;
            } else {
                s_tube_settings = *settings_get();
                s_tube_field = 0;
                s_tube_editing = false;
                s_screen = LCD_SCREEN_TUBE;
            }
        } else if (input == BOARD_IN_BTN_LEFT) {
            s_menu_item = s_menu_item == 0 ? 3 : s_menu_item - 1;
        } else if (input == BOARD_IN_BTN_RIGHT) {
            s_menu_item = s_menu_item == 3 ? 0 : s_menu_item + 1;
        }
        break;
    case LCD_SCREEN_WIFI:
        if (input == BOARD_IN_BTN_LEFT) {
            s_screen = LCD_SCREEN_MENU;
        }
        break;
    case LCD_SCREEN_MQTT:
        if (input == BOARD_IN_BTN_LEFT) {
            s_screen = LCD_SCREEN_MENU;
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
    }
}

static void main_screen_task(void *arg)
{
    for (;;) {
        switch (s_screen) {
        case LCD_SCREEN_MAIN:
            draw_main_screen();
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
        case LCD_SCREEN_TUBE:
            draw_tube_screen();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t lcd_start_main_screen(void)
{
    ESP_RETURN_ON_FALSE(xTaskCreate(main_screen_task, "display", 3072, NULL, 4, NULL) == pdPASS,
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
    const int icon_y = 32;
    u8g2_DrawCircle(display, icon_x, icon_y, 16, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x, icon_y, 4, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x, icon_y - 10, 4, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x - 9, icon_y + 5, 4, U8G2_DRAW_ALL);
    u8g2_DrawDisc(display, icon_x + 9, icon_y + 5, 4, U8G2_DRAW_ALL);

    u8g2_SetFont(display, u8g2_font_5x7_tf);
    u8g2_DrawStr(display, 34, 61, "INITIALIZING");
    u8g2_SendBuffer(display);
}