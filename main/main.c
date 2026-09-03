#include "config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "geiger.h"
#include "history_store.h"
#include "lcd.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "ota.h"
#include "settings.h"
#include "wifi_prov.h"

static const char *TAG = "app";

static TaskHandle_t     s_tube_tick_task;
static esp_timer_handle_t s_tick_stop_timer;

static void tick_stop_timer_cb(void *arg)
{
    board_buzzer_off();
    board_set_led(false);
}

static uint32_t buzzer_click_duration_us(void)
{
    switch ((sound_type_t)settings_get()->spk_sound) {
    case SOUND_SHORT:
        return BUZZER_CLICK_SHORT_MS * 1000;
    case SOUND_LONG:
        return BUZZER_CLICK_LONG_MS * 1000;
    case SOUND_NORMAL:
    default:
        return BUZZER_CLICK_NORMAL_MS * 1000;
    }
}

static void buzzer_click(void)
{
    board_buzzer_on(PWM_BUZZER_FREQ_HZ, settings_get()->spk_volume);
    if (esp_timer_is_active(s_tick_stop_timer)) {
        esp_timer_stop(s_tick_stop_timer);
    }
    esp_timer_start_once(s_tick_stop_timer, buzzer_click_duration_us());
}

static void tube_pulse_feedback(void)
{
    bool feedback_active = false;

    if (!lcd_lamp_test_active()) {
        board_buzzer_on(PWM_BUZZER_FREQ_HZ, settings_get()->spk_volume);
        feedback_active = true;
    }
    if (settings_get()->led_enabled) {
        board_set_led(true);
        feedback_active = true;
    }
    if (!feedback_active) {
        return;
    }
    if (esp_timer_is_active(s_tick_stop_timer)) {
        esp_timer_stop(s_tick_stop_timer);
    }
    esp_timer_start_once(s_tick_stop_timer, buzzer_click_duration_us());
}

static void tube_pulse_isr(board_input_t input, bool level, void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    vTaskNotifyGiveFromISR(s_tube_tick_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void tube_tick_task(void *arg)
{
    for (;;) {
        uint32_t pulses = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Tube tick detected (%lu pulse%s)", (unsigned long)pulses,
                 pulses == 1 ? "" : "s");
        tube_pulse_feedback();
    }
}

static esp_err_t tube_tick_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = tick_stop_timer_cb,
        .name = "tick_stop",
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_tick_stop_timer), TAG, "tick timer");
    ESP_RETURN_ON_FALSE(xTaskCreate(tube_tick_task, "tube_tick", 2048, NULL, 5,
                                    &s_tube_tick_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "tick task");
    return board_input_set_isr(BOARD_IN_TUBE_CNT, tube_pulse_isr, NULL);
}

typedef struct {
    board_input_t input;
    bool          level;
} button_event_t;

#define POWER_OFF_HOLD_US (3ULL * 1000 * 1000)
#define POWER_ON_HOLD_US  (3LL * 1000 * 1000)
#define STANDBY_BACKLIGHT_US (3LL * 1000 * 1000)
#define STANDBY_CLEAR_US (3LL * 1000 * 1000)
#define POWER_ON_POLL_MS 20
#define POWER_ON_FRAME_US (200LL * 1000)

static QueueHandle_t s_button_events;
static esp_timer_handle_t s_power_off_timer;

static void power_off_timer_cb(void *arg)
{
    if (!board_input_level(BOARD_IN_BTN_ENTER)) {
        ESP_LOGI(TAG, "Enter held for 3 seconds, requesting shutdown confirmation");
        lcd_request_shutdown_confirmation();
    }
}

static void power_off(void)
{
    ESP_LOGI(TAG, "Shutdown confirmed");
    esp_err_t hv_err = board_hv_set_enabled(false);
    if (hv_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable high voltage: %s", esp_err_to_name(hv_err));
    }
    board_buzzer_off();
    lcd_fade_out_and_clear();
    board_set_latch(false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void power_off_hold_update(const button_event_t *event)
{
    if (event->input != BOARD_IN_BTN_ENTER) {
        return;
    }

    if (esp_timer_is_active(s_power_off_timer)) {
        esp_timer_stop(s_power_off_timer);
    }
    if (!event->level) {
        esp_timer_start_once(s_power_off_timer, POWER_OFF_HOLD_US);
    }
}

static const char *button_name(board_input_t input)
{
    switch (input) {
    case BOARD_IN_BTN_ENTER:
        return "ENTER";
    case BOARD_IN_BTN_LEFT:
        return "LEFT";
    case BOARD_IN_BTN_RIGHT:
        return "RIGHT";
    default:
        return "UNKNOWN";
    }
}

esp_err_t button_simulate(board_input_t input)
{
    ESP_RETURN_ON_FALSE(s_button_events, ESP_ERR_INVALID_STATE, TAG, "button queue not ready");
    ESP_RETURN_ON_FALSE(input >= BOARD_IN_BTN_ENTER && input <= BOARD_IN_BTN_RIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "not a button");

    button_event_t pressed = { .input = input, .level = false };
    button_event_t released = { .input = input, .level = true };
    ESP_RETURN_ON_FALSE(xQueueSend(s_button_events, &pressed, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "button queue full");
    ESP_RETURN_ON_FALSE(xQueueSend(s_button_events, &released, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "button queue full");
    return ESP_OK;
}

static void button_state_changed_isr(board_input_t input, bool level, void *arg)
{
    button_event_t event = { .input = input, .level = level };
    BaseType_t higher_priority_task_woken = pdFALSE;

    xQueueSendFromISR(s_button_events, &event, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void button_event_task(void *arg)
{
    button_event_t event;

    for (;;) {
        xQueueReceive(s_button_events, &event, portMAX_DELAY);
        ESP_LOGI(TAG, "button %s %s", button_name(event.input),
                 event.level ? "released" : "pressed");
        power_off_hold_update(&event);

        bool lamp_test_was_active = lcd_lamp_test_active();
        bool shutdown_confirmed = lcd_handle_button(event.input, !event.level);
        bool lamp_test_is_active = lcd_lamp_test_active();

        if (shutdown_confirmed) {
            power_off();
        }

        if (lamp_test_is_active) {
            if (esp_timer_is_active(s_tick_stop_timer)) {
                esp_timer_stop(s_tick_stop_timer);
            }
            board_buzzer_on(PWM_BUZZER_FREQ_HZ, 100);
        } else if (lamp_test_was_active) {
            board_buzzer_off();
        } else if (!event.level) {
            buzzer_click();
        }
    }
}

static esp_err_t button_events_init(void)
{
    const esp_timer_create_args_t power_off_timer_args = {
        .callback = power_off_timer_cb,
        .name = "power_off_hold",
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&power_off_timer_args, &s_power_off_timer),
                        TAG, "power off timer");
    s_button_events = xQueueCreate(16, sizeof(button_event_t));
    ESP_RETURN_ON_FALSE(s_button_events, ESP_ERR_NO_MEM, TAG, "button event queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(button_event_task, "buttons", 2048, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "button task");

    board_input_set_isr(BOARD_IN_BTN_ENTER, button_state_changed_isr, NULL);
    board_input_set_isr(BOARD_IN_BTN_LEFT, button_state_changed_isr, NULL);
    board_input_set_isr(BOARD_IN_BTN_RIGHT, button_state_changed_isr, NULL);
    return ESP_OK;
}

static void nvs_bringup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void power_management_init(void)
{
    const esp_pm_config_t config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };

    ESP_ERROR_CHECK(esp_pm_configure(&config));
    ESP_LOGI(TAG, "Power management enabled: CPU 40-160 MHz, automatic light sleep");
}

static void wait_for_power_on(void)
{
    ESP_LOGI(TAG, "Waiting for a 3 second Enter hold");
    int64_t pressed_since_us = 0;
    int64_t backlight_off_at_us = esp_timer_get_time() + STANDBY_BACKLIGHT_US;
    int64_t clear_at_us = backlight_off_at_us + STANDBY_CLEAR_US;
    int64_t next_frame_at_us = 0;
    uint8_t animation_frame = 0;
    uint8_t breath_phase = 0;
    uint8_t breath_accumulator = 0;
    bool backlight_on = true;
    bool screen_visible = true;
    bool enter_was_pressed = false;

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        bool enter_pressed = !board_input_level(BOARD_IN_BTN_ENTER);

        uint8_t breath_level = breath_phase <= 100 ? breath_phase : 200 - breath_phase;
        breath_accumulator += breath_level;
        board_set_led(breath_accumulator >= 100);
        if (breath_accumulator >= 100) {
            breath_accumulator -= 100;
        }
        breath_phase = breath_phase == 199 ? 0 : breath_phase + 1;

        if (enter_pressed != enter_was_pressed) {
            ESP_LOGI(TAG, "Startup Enter %s", enter_pressed ? "pressed" : "released");
            enter_was_pressed = enter_pressed;
            if (enter_pressed) {
                lcd_wake_backlight();
                backlight_on = true;
                screen_visible = true;
                next_frame_at_us = 0;
                backlight_off_at_us = now_us + STANDBY_BACKLIGHT_US;
                clear_at_us = backlight_off_at_us + STANDBY_CLEAR_US;
            } else {
                backlight_off_at_us = now_us + STANDBY_BACKLIGHT_US;
                clear_at_us = backlight_off_at_us + STANDBY_CLEAR_US;
            }
        }

        if (enter_pressed) {
            if (pressed_since_us == 0) {
                pressed_since_us = now_us;
            } else if (now_us - pressed_since_us >= POWER_ON_HOLD_US) {
                ESP_LOGI(TAG, "Enter held for 3 seconds, continuing startup");
                board_set_led(false);
                lcd_set_backlight(settings_get()->lcd_brightness);
                return;
            }
        } else {
            pressed_since_us = 0;
        }

        if (backlight_on && !enter_pressed && now_us >= backlight_off_at_us) {
            lcd_set_backlight(0);
            backlight_on = false;
        }

        if (screen_visible && !enter_pressed && now_us >= clear_at_us) {
            lcd_clear();
            screen_visible = false;
        }

        if (screen_visible && now_us >= next_frame_at_us) {
            bool charging_high = board_input_level(BOARD_IN_CHRG);
            bool standby_high = board_input_level(BOARD_IN_STBY);
            lcd_battery_state_t battery_state;

            if (charging_high && standby_high) {
                battery_state = LCD_BATTERY_FAULT;
            } else if (!standby_high) {
                battery_state = LCD_BATTERY_CHARGED;
            } else {
                battery_state = LCD_BATTERY_CHARGING;
            }
            lcd_draw_battery_status(battery_state, animation_frame++);
            next_frame_at_us = now_us + POWER_ON_FRAME_US;
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_ON_POLL_MS));
    }
}

void app_main(void)
{
    power_management_init();
    nvs_bringup();
    ESP_ERROR_CHECK(settings_init());   /* everything below reads settings_get() */
    ESP_ERROR_CHECK(history_store_init());
    ESP_ERROR_CHECK(ota_init());

    ESP_ERROR_CHECK(board_init());
    board_apply_settings();
    ESP_ERROR_CHECK(lcd_backlight_init());
    wait_for_power_on();
    lcd_draw_startup_screen();

    ESP_ERROR_CHECK(board_hv_set_freq(PWM_TUBE_FREQ_HZ));
    ESP_ERROR_CHECK(board_hv_set_duty(PWM_TUBE_STARTUP_DUTY_PCT));
    ESP_ERROR_CHECK(board_hv_set_enabled(settings_get()->hv_start_enabled));
    ESP_ERROR_CHECK(board_hv_regulator_start());
    ESP_ERROR_CHECK(tube_tick_init());
    ESP_ERROR_CHECK(button_events_init());
    ESP_ERROR_CHECK(geiger_start());
    ESP_ERROR_CHECK(wifi_prov_init());
    ESP_ERROR_CHECK(mqtt_init());

    wifi_prov_method_t provisioning = WIFI_PROV_BLUFI;
    if (!wifi_has_credentials()) {
        provisioning |= WIFI_PROV_SOFTAP;
        ESP_LOGI(TAG, "provisioning: BluFi over BLE, or join \"%s\"", wifi_softap_ssid());
    } else {
        ESP_LOGI(TAG, "BluFi ready for Wi-Fi reconfiguration");
    }
    ESP_ERROR_CHECK(wifi_prov_start(provisioning));
    ESP_ERROR_CHECK(lcd_start_main_screen());
}
