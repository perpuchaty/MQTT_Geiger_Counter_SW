#include "lcd.h"

#include <stdio.h>
#include <time.h>

#include "config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "geiger.h"
#include "wifi_prov.h"

static const char *TAG = "lcd";

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

static void main_screen_task(void *arg)
{
    for (;;) {
        draw_main_screen();
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