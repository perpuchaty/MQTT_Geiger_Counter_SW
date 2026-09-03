#include "config.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/adc_types.h"
#include "settings.h"
#include "soc/soc_caps.h"

static const char *TAG = "board";

/* =========================================================================
 * GPIO
 * ====================================================================== */

/* Kept in RAM so the ISR never touches flash. */
static const gpio_num_t s_in_pin[BOARD_IN_COUNT] = {
    [BOARD_IN_CHRG]      = PIN_CHRG,
    [BOARD_IN_STBY]      = PIN_STBY,
    [BOARD_IN_TUBE_CNT]  = PIN_TUBE_CNT,
    [BOARD_IN_VTUBE_OK]  = PIN_VTUBE_OK,
    [BOARD_IN_BTN_ENTER] = PIN_BUTTON_ENTER,
    [BOARD_IN_BTN_LEFT]  = PIN_BUTTON_LEFT,
    [BOARD_IN_BTN_RIGHT] = PIN_BUTTON_RIGHT,
};

static board_input_isr_t s_in_cb[BOARD_IN_COUNT];
static void             *s_in_arg[BOARD_IN_COUNT];
static volatile uint32_t s_tube_pulses;
static portMUX_TYPE      s_in_lock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR board_gpio_isr(void *arg)
{
    board_input_t in = (board_input_t)(uintptr_t)arg;
    bool level = gpio_get_level(s_in_pin[in]);

    if (in == BOARD_IN_TUBE_CNT) {
        s_tube_pulses++;
    }
    if (s_in_cb[in]) {
        s_in_cb[in](in, level, s_in_arg[in]);
    }
}

static esp_err_t gpio_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_LATCH) | BIT64(PIN_CHARGE_EN) | BIT64(PIN_LED) |
                        BIT64(PIN_LCD_RESET) | BIT64(PIN_LCD_A0),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
        ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "output config failed");

    gpio_set_level(PIN_LATCH, 0);
    gpio_set_level(PIN_CHARGE_EN, 0);
    gpio_set_level(PIN_LED, 0);
    gpio_set_level(PIN_LCD_RESET, 0);
    gpio_set_level(PIN_LCD_A0, 0);

    gpio_config_t inputs_pullup = {
        .pin_bit_mask = BIT64(PIN_CHRG) | BIT64(PIN_STBY) | BIT64(PIN_TUBE_CNT) |
                        BIT64(PIN_BUTTON_ENTER) | BIT64(PIN_BUTTON_LEFT) | BIT64(PIN_BUTTON_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs_pullup), TAG, "pull-up input config failed");

    gpio_config_t input_no_pull = {
        .pin_bit_mask = BIT64(PIN_VTUBE_OK),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_no_pull), TAG, "tube voltage input config failed");

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    gpio_set_intr_type(PIN_CHRG, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_CHRG, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_CHRG);

    gpio_set_intr_type(PIN_STBY, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_STBY, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_STBY);

    gpio_set_intr_type(PIN_TUBE_CNT, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(PIN_TUBE_CNT, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_TUBE_CNT);

    gpio_set_intr_type(PIN_VTUBE_OK, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_VTUBE_OK, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_VTUBE_OK);

    gpio_set_intr_type(PIN_BUTTON_ENTER, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_BUTTON_ENTER, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_BTN_ENTER);

    gpio_set_intr_type(PIN_BUTTON_LEFT, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_BUTTON_LEFT, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_BTN_LEFT);
    
    gpio_set_intr_type(PIN_BUTTON_RIGHT, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_BUTTON_RIGHT, board_gpio_isr, (void *)(uintptr_t)BOARD_IN_BTN_RIGHT);
    return ESP_OK;
}

void board_set_latch(bool on)     { gpio_set_level(PIN_LATCH, on); }
void board_set_charge_en(bool on) { gpio_set_level(PIN_CHARGE_EN, on); }
void board_set_led(bool on)       { gpio_set_level(PIN_LED, on); }

bool board_input_level(board_input_t in)
{
    if (in >= BOARD_IN_COUNT) {
        return false;
    }
    return gpio_get_level(s_in_pin[in]) != 0;
}

esp_err_t board_input_set_isr(board_input_t in, board_input_isr_t cb, void *arg)
{
    ESP_RETURN_ON_FALSE(in < BOARD_IN_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad input");
    portENTER_CRITICAL(&s_in_lock);
    s_in_arg[in] = arg;
    s_in_cb[in]  = cb;
    portEXIT_CRITICAL(&s_in_lock);
    return ESP_OK;
}

uint32_t board_tube_pulses(void)
{
    return s_tube_pulses;
}

void board_simulate_tube_pulse(void)
{
    s_tube_pulses++;
}

/* =========================================================================
 * PWM (LEDC)
 * ====================================================================== */

static float s_hv_duty_pct;
static bool s_hv_enabled;

static uint32_t duty_from_pct(ledc_timer_bit_t res, float pct)
{
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint32_t)(((1u << (uint32_t)res) - 1u) * (pct / 100.0f) + 0.5f);
}

static esp_err_t ledc_setup(ledc_timer_t timer, ledc_channel_t ch, gpio_num_t pin,
                            ledc_timer_bit_t res, uint32_t freq)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = PWM_SPEED_MODE,
        .timer_num       = timer,
        .duty_resolution = res,
        .freq_hz         = freq,
        .clk_cfg         = PWM_CLK_SRC,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer %d failed", timer);

    ledc_channel_config_t ccfg = {
        .speed_mode = PWM_SPEED_MODE,
        .channel    = ch,
        .timer_sel  = timer,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = pin,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&ccfg);
}

static esp_err_t pwm_init(void)
{
    ESP_RETURN_ON_ERROR(ledc_setup(PWM_TUBE_TIMER, PWM_TUBE_CHANNEL, PIN_PWM_TUBE,
                                   PWM_TUBE_RES, PWM_TUBE_FREQ_HZ), TAG, "hv pwm");
    ESP_RETURN_ON_ERROR(ledc_setup(PWM_BACKLIGHT_TIMER, PWM_BACKLIGHT_CHANNEL, PIN_PWM_LCD,
                                   PWM_BACKLIGHT_RES, PWM_BACKLIGHT_FREQ_HZ), TAG, "backlight pwm");
    ESP_RETURN_ON_ERROR(ledc_setup(PWM_BUZZER_TIMER, PWM_BUZZER_CHANNEL, PIN_PWM_BUZZER,
                                   PWM_BUZZER_RES, PWM_BUZZER_FREQ_HZ), TAG, "buzzer pwm");
    return ESP_OK;
}

static esp_err_t pwm_apply(ledc_channel_t ch, uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(PWM_SPEED_MODE, ch, duty), TAG, "set duty");
    return ledc_update_duty(PWM_SPEED_MODE, ch);
}

esp_err_t board_hv_set_duty(float duty_pct)
{
    if (duty_pct < 0.0f) {
        duty_pct = 0.0f;
    }
    if (duty_pct > PWM_TUBE_DUTY_MAX_PCT) {
        duty_pct = PWM_TUBE_DUTY_MAX_PCT;
    }
    ESP_RETURN_ON_ERROR(pwm_apply(PWM_TUBE_CHANNEL,
                                  duty_from_pct(PWM_TUBE_RES, s_hv_enabled ? duty_pct : 0.0f)),
                        TAG, "hv duty");
    s_hv_duty_pct = duty_pct;
    return ESP_OK;
}

esp_err_t board_hv_set_enabled(bool enabled)
{
    s_hv_enabled = enabled;
    return pwm_apply(PWM_TUBE_CHANNEL,
                     duty_from_pct(PWM_TUBE_RES, enabled ? s_hv_duty_pct : 0.0f));
}

bool board_hv_is_enabled(void)
{
    return s_hv_enabled;
}

esp_err_t board_hv_set_freq(uint32_t freq_hz)
{
    ESP_RETURN_ON_FALSE(freq_hz >= 100 && freq_hz <= 100000, ESP_ERR_INVALID_ARG, TAG, "hv frequency");
    ESP_RETURN_ON_ERROR(ledc_set_freq(PWM_SPEED_MODE, PWM_TUBE_TIMER, freq_hz), TAG, "hv frequency");
    return ESP_OK;
}

float board_hv_duty_pct(void)
{
    return s_hv_duty_pct;
}

float board_hv_output_duty_pct(void)
{
    const uint32_t max_duty = (1u << (uint32_t)PWM_TUBE_RES) - 1u;
    return ledc_get_duty(PWM_SPEED_MODE, PWM_TUBE_CHANNEL) * 100.0f / max_duty;
}

uint32_t board_hv_freq_hz(void)
{
    return ledc_get_freq(PWM_SPEED_MODE, PWM_TUBE_TIMER);
}

esp_err_t board_pwm_get_status(board_pwm_t pwm, board_pwm_status_t *status)
{
    static const ledc_timer_t timers[BOARD_PWM_COUNT] = {
        [BOARD_PWM_TUBE] = PWM_TUBE_TIMER,
        [BOARD_PWM_LCD] = PWM_BACKLIGHT_TIMER,
        [BOARD_PWM_BUZZER] = PWM_BUZZER_TIMER,
    };
    static const ledc_channel_t channels[BOARD_PWM_COUNT] = {
        [BOARD_PWM_TUBE] = PWM_TUBE_CHANNEL,
        [BOARD_PWM_LCD] = PWM_BACKLIGHT_CHANNEL,
        [BOARD_PWM_BUZZER] = PWM_BUZZER_CHANNEL,
    };
    static const ledc_timer_bit_t resolutions[BOARD_PWM_COUNT] = {
        [BOARD_PWM_TUBE] = PWM_TUBE_RES,
        [BOARD_PWM_LCD] = PWM_BACKLIGHT_RES,
        [BOARD_PWM_BUZZER] = PWM_BUZZER_RES,
    };

    ESP_RETURN_ON_FALSE(pwm < BOARD_PWM_COUNT && status, ESP_ERR_INVALID_ARG, TAG, "bad pwm");
    uint32_t max_duty = (1u << (uint32_t)resolutions[pwm]) - 1u;
    status->freq_hz = ledc_get_freq(PWM_SPEED_MODE, timers[pwm]);
    status->duty_pct = ledc_get_duty(PWM_SPEED_MODE, channels[pwm]) * 100.0f / max_duty;
    return ESP_OK;
}

static void hv_regulator_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HV_REGULATOR_INTERVAL_MS));
        if (!board_hv_is_enabled()) {
            continue;
        }

        int voltage_mv;
        if (board_tube_voltage_get_mv(&voltage_mv) != ESP_OK) {
            continue;
        }

        int target_mv = settings_get()->tube_target_v * 1000;
        float duty_pct = board_hv_duty_pct();
        if (voltage_mv < target_mv - HV_REGULATOR_DEADBAND_MV) {
            board_hv_set_duty(duty_pct + HV_REGULATOR_STEP_PCT);
        } else if (voltage_mv > target_mv + HV_REGULATOR_DEADBAND_MV) {
            board_hv_set_duty(duty_pct - HV_REGULATOR_STEP_PCT);
        }
    }
}

esp_err_t board_hv_regulator_start(void)
{
    return xTaskCreate(hv_regulator_task, "hv_regulator", 2048, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t board_backlight_set(uint8_t duty_pct)
{
    return pwm_apply(PWM_BACKLIGHT_CHANNEL, duty_from_pct(PWM_BACKLIGHT_RES, duty_pct));
}

esp_err_t board_buzzer_on(uint32_t freq_hz, uint8_t duty_level)
{
    if (freq_hz) {
        ESP_RETURN_ON_ERROR(ledc_set_freq(PWM_SPEED_MODE, PWM_BUZZER_TIMER, freq_hz), TAG, "buzzer freq");
    }
    if (duty_level > 200) {
        duty_level = 200;
    }
    return pwm_apply(PWM_BUZZER_CHANNEL,
                     duty_from_pct(PWM_BUZZER_RES, duty_level / 2.0f));
}

esp_err_t board_buzzer_off(void)
{
    return pwm_apply(PWM_BUZZER_CHANNEL, 0);
}

/* =========================================================================
 * ADC - continuous conversion + calibration
 * ====================================================================== */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t       s_adc_cali[BOARD_ADC_CH_COUNT];
static adc_channel_t           s_adc_chan[BOARD_ADC_CH_COUNT];
static volatile int            s_adc_raw[BOARD_ADC_CH_COUNT];

static const gpio_num_t s_adc_pin[BOARD_ADC_CH_COUNT] = {
    [BOARD_ADC_VLATCH] = PIN_ADC_VLATCH,
    [BOARD_ADC_TUBE]   = PIN_ADC_TUBE,
};

static esp_err_t adc_cali_create(adc_unit_t unit, adc_channel_t chan, adc_cali_handle_t *out)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = unit,
        .chan     = chan,
        .atten    = BOARD_ADC_ATTEN,
        .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    return adc_cali_create_scheme_curve_fitting(&cfg, out);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id  = unit,
        .atten    = BOARD_ADC_ATTEN,
        .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    return adc_cali_create_scheme_line_fitting(&cfg, out);
#else
    *out = NULL;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void adc_sample_burst(void)
{
    for (int channel = 0; channel < BOARD_ADC_CH_COUNT; channel++) {
        int sum = 0;
        int samples = 0;

        for (int sample = 0; sample < BOARD_ADC_BURST_SAMPLES; sample++) {
            int raw;
            if (adc_oneshot_read(s_adc, s_adc_chan[channel], &raw) == ESP_OK) {
                sum += raw;
                samples++;
            }
        }

        if (samples > 0) {
            s_adc_raw[channel] = (sum + samples / 2) / samples;
        }
    }
}

static void adc_task(void *arg)
{
    TickType_t next_sample = xTaskGetTickCount();

    for (;;) {
        adc_sample_burst();
        vTaskDelayUntil(&next_sample, pdMS_TO_TICKS(BOARD_ADC_INTERVAL_MS));
    }
}

static esp_err_t adc_init(void)
{
    adc_unit_t unit = ADC_UNIT_1;

    for (int c = 0; c < BOARD_ADC_CH_COUNT; c++) {
        adc_unit_t u;
        ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(s_adc_pin[c], &u, &s_adc_chan[c]),
                            TAG, "GPIO%d is not an ADC pin", s_adc_pin[c]);
        ESP_RETURN_ON_FALSE(c == 0 || u == unit, ESP_ERR_INVALID_ARG, TAG,
                            "all ADC pins must belong to the same unit");
        unit = u;

        if (adc_cali_create(unit, s_adc_chan[c], &s_adc_cali[c]) != ESP_OK) {
            ESP_LOGW(TAG, "no eFuse calibration for ch %d, raw values only", c);
        }
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc handle");

    adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = BOARD_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    for (int c = 0; c < BOARD_ADC_CH_COUNT; c++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_adc_chan[c], &channel_cfg),
                            TAG, "adc channel %d", c);
    }

    adc_sample_burst();

    return xTaskCreate(adc_task, "adc", 2048, NULL, 6, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t board_adc_get_raw(board_adc_ch_t ch, int *raw)
{
    ESP_RETURN_ON_FALSE(ch < BOARD_ADC_CH_COUNT && raw, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    *raw = s_adc_raw[ch];
    return ESP_OK;
}

esp_err_t board_adc_get_mv(board_adc_ch_t ch, int *mv)
{
    int raw;
    int pin_mv;
    ESP_RETURN_ON_ERROR(board_adc_get_raw(ch, &raw), TAG, "raw");
    ESP_RETURN_ON_FALSE(s_adc_cali[ch], ESP_ERR_NOT_SUPPORTED, TAG, "not calibrated");
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali[ch], raw, &pin_mv),
                        TAG, "calibrate voltage");

    if (ch == BOARD_ADC_VLATCH) {
        *mv = (int)(((int64_t)pin_mv * VLATCH_DIVIDER_NUMERATOR +
                     VLATCH_DIVIDER_DENOMINATOR / 2) /
                    VLATCH_DIVIDER_DENOMINATOR);
    } else {
        *mv = pin_mv;
    }
    return ESP_OK;
}

esp_err_t board_tube_voltage_get_mv(int *mv)
{
    int divider_mv;
    ESP_RETURN_ON_FALSE(mv, ESP_ERR_INVALID_ARG, TAG, "null voltage output");
    ESP_RETURN_ON_ERROR(board_adc_get_mv(BOARD_ADC_TUBE, &divider_mv), TAG, "tube divider voltage");

    *mv = (int)(((int64_t)divider_mv * (TUBE_DIVIDER_TOP_OHM + TUBE_DIVIDER_BOTTOM_OHM) +
                 TUBE_DIVIDER_BOTTOM_OHM / 2) / TUBE_DIVIDER_BOTTOM_OHM);
    return ESP_OK;
}

/* =========================================================================
 * LCD - ST7565P on hardware SPI, driven through u8g2
 * ====================================================================== */

#define LCD_TX_BUF_SIZE 256

static spi_device_handle_t s_lcd_spi;
static u8g2_t              s_u8g2;
static uint8_t            *s_lcd_tx;
static size_t              s_lcd_tx_len;

static void lcd_flush(void)
{
    if (!s_lcd_tx_len) {
        return;
    }
    spi_transaction_t t = {
        .length    = s_lcd_tx_len * 8,
        .tx_buffer = s_lcd_tx,
    };
    spi_device_polling_transmit(s_lcd_spi, &t);
    s_lcd_tx_len = 0;
}

static uint8_t u8x8_byte_esp_spi(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_BYTE_SEND: {
        const uint8_t *data = arg_ptr;
        while (arg_int--) {
            if (s_lcd_tx_len >= LCD_TX_BUF_SIZE) {
                lcd_flush();
            }
            s_lcd_tx[s_lcd_tx_len++] = *data++;
        }
        break;
    }
    case U8X8_MSG_BYTE_SET_DC:
        lcd_flush();  /* A0 may only change between transfers */
            gpio_set_level(PIN_LCD_A0, arg_int);
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
        lcd_flush();
        break;
    case U8X8_MSG_BYTE_INIT:
    case U8X8_MSG_BYTE_START_TRANSFER:
        break;
    default:
        return 0;
    }
    return 1;
}

static uint8_t u8x8_gpio_and_delay_esp(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        break;
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int ? arg_int : 1));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(10 * arg_int);
        break;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        break;
    case U8X8_MSG_GPIO_RESET:
            gpio_set_level(PIN_LCD_RESET, arg_int);
        break;
    case U8X8_MSG_GPIO_DC:
            gpio_set_level(PIN_LCD_A0, arg_int);
        break;
    case U8X8_MSG_GPIO_CS:
        break;  /* driven by the SPI peripheral */
    default:
        return 0;
    }
    return 1;
}

static esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_LCD_DATA0,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_LCD_CLOCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_TX_BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_CLOCK_HZ,
        .mode           = LCD_SPI_MODE,
        .spics_io_num   = PIN_LCD_CS,
        .queue_size     = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev, &s_lcd_spi), TAG, "spi device");

    s_lcd_tx = heap_caps_malloc(LCD_TX_BUF_SIZE, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_lcd_tx, ESP_ERR_NO_MEM, TAG, "spi tx buffer");

    LCD_U8G2_SETUP(&s_u8g2, LCD_U8G2_ROTATION, u8x8_byte_esp_spi, u8x8_gpio_and_delay_esp);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetContrast(&s_u8g2, LCD_CONTRAST_DEFAULT);
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SendBuffer(&s_u8g2);
    return ESP_OK;
}

u8g2_t *board_lcd(void)
{
    return &s_u8g2;
}

/* =========================================================================
 * Entry point
 * ====================================================================== */

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(gpio_init(), TAG, "gpio init failed");
    board_set_latch(true);  /* keep the power latch closed while running */

    ESP_RETURN_ON_ERROR(pwm_init(), TAG, "pwm init failed");
    ESP_RETURN_ON_ERROR(adc_init(), TAG, "adc init failed");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd init failed");

    ESP_LOGI(TAG, "board ready");
    return ESP_OK;
}

void board_apply_settings(void)
{
    const settings_t *cfg = settings_get();

    board_set_charge_en(cfg->batt_charge_en);
    if (!cfg->led_enabled) {
        board_set_led(false);
    }
}
