#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Pinout
 * ---------------------------------------------------------------------- */
#define TARGET_CPU ESP32C6

#if defined(TARGET_CPU) && TARGET_CPU == ESP32C6
#define PIN_LATCH GPIO_NUM_0 //output
#define PIN_CHARGE_EN GPIO_NUM_1 //output
#define PIN_CHRG GPIO_NUM_2 //input
#define PIN_STBY GPIO_NUM_3 //input
#define PIN_ADC_VLATCH GPIO_NUM_4 //input ADC
#define PIN_PWM_TUBE GPIO_NUM_5 //output PWM
#define PIN_ADC_TUBE GPIO_NUM_6 //input ADC
#define PIN_TUBE_CNT GPIO_NUM_7 //input pulse counter 
#define PIN_VTUBE_OK GPIO_NUM_8 //input
#define PIN_LED GPIO_NUM_9  //output
#define PIN_PWM_LCD GPIO_NUM_14 //output PWM
#define PIN_BUTTON_ENTER GPIO_NUM_15 //input
#define PIN_BUTTON_LEFT GPIO_NUM_16 //input
#define PIN_BUTTON_RIGHT GPIO_NUM_17 //input
#define PIN_PWM_BUZZER GPIO_NUM_18 //output pwm
#define PIN_LCD_CS GPIO_NUM_19  //output lcd ST7565P
#define PIN_LCD_RESET GPIO_NUM_20//output lcd ST7565P
#define PIN_LCD_A0 GPIO_NUM_21//output lcd ST7565P
#define PIN_LCD_DATA0 GPIO_NUM_22//output lcd ST7565P
#define PIN_LCD_CLOCK GPIO_NUM_23//output lcd ST7565P
#endif


/* -------------------------------------------------------------------------
 * LCD - ST7565P over hardware SPI (u8g2, full framebuffer mode)
 * ---------------------------------------------------------------------- */
#define LCD_SPI_HOST            SPI2_HOST          /* GPSPI2: the only general purpose host on C6 */
#define LCD_SPI_CLOCK_HZ        (1 * 1000 * 1000)
#define LCD_SPI_MODE            0
#define LCD_WIDTH               128
#define LCD_HEIGHT              64
#define LCD_U8G2_SETUP          u8g2_Setup_st7565_erc12864_alt_f
#define LCD_U8G2_ROTATION       U8G2_R0
#define LCD_CONTRAST_DEFAULT    32              /* tune for the fitted LCD panel */

/* -------------------------------------------------------------------------
 * PWM (LEDC). ESP32-C6 only implements the low speed mode.
 * ---------------------------------------------------------------------- */
#define PWM_SPEED_MODE          LEDC_LOW_SPEED_MODE
#define PWM_CLK_SRC             LEDC_USE_PLL_DIV_CLK

/* Geiger tube HV boost converter */
#define PWM_TUBE_TIMER          LEDC_TIMER_0
#define PWM_TUBE_CHANNEL        LEDC_CHANNEL_0
#define PWM_TUBE_RES            LEDC_TIMER_10_BIT
#define PWM_TUBE_FREQ_HZ        1000
#define PWM_TUBE_STARTUP_DUTY_PCT 30.0f
#define PWM_TUBE_DUTY_MAX_PCT   35.0f              /* hard limit, protects inductor and FET */
#define HV_REGULATOR_INTERVAL_MS 500
#define HV_REGULATOR_STEP_PCT   1.0f
#define HV_REGULATOR_DEADBAND_MV 5000

/* LCD backlight */
#define PWM_BACKLIGHT_TIMER     LEDC_TIMER_1
#define PWM_BACKLIGHT_CHANNEL   LEDC_CHANNEL_1
#define PWM_BACKLIGHT_RES       LEDC_TIMER_10_BIT
#define PWM_BACKLIGHT_FREQ_HZ   5000

/* Buzzer, frequency is retuned at runtime */
#define PWM_BUZZER_TIMER        LEDC_TIMER_2
#define PWM_BUZZER_CHANNEL      LEDC_CHANNEL_2
#define PWM_BUZZER_RES          LEDC_TIMER_10_BIT
#define PWM_BUZZER_FREQ_HZ      2500
#define BUZZER_CLICK_SHORT_MS    5
#define BUZZER_CLICK_NORMAL_MS   20
#define BUZZER_CLICK_LONG_MS     50

/* -------------------------------------------------------------------------
 * ADC - periodic one-shot bursts with hardware calibration
 * ---------------------------------------------------------------------- */
#define BOARD_ADC_ATTEN         ADC_ATTEN_DB_12    /* full ~0..3.1 V input range */
#define BOARD_ADC_BURST_SAMPLES 32
#define BOARD_ADC_INTERVAL_MS   500
#define VLATCH_DIVIDER_NUMERATOR   2
#define VLATCH_DIVIDER_DENOMINATOR 1
#define TUBE_DIVIDER_TOP_OHM    80000000UL
#define TUBE_DIVIDER_BOTTOM_OHM 510000UL

typedef enum {
    BOARD_ADC_VLATCH = 0,   /* PIN_ADC_VLATCH - battery / latch rail */
    BOARD_ADC_TUBE,         /* PIN_ADC_TUBE   - HV feedback divider */
    BOARD_ADC_CH_COUNT,
} board_adc_ch_t;

/* -------------------------------------------------------------------------
 * Interrupt driven inputs
 * ---------------------------------------------------------------------- */
typedef enum {
    BOARD_IN_CHRG = 0,      /* charger charging, active low */
    BOARD_IN_STBY,          /* charger standby,  active low */
    BOARD_IN_TUBE_CNT,      /* Geiger tube pulse, falling edge */
    BOARD_IN_VTUBE_OK,      /* HV comparator */
    BOARD_IN_BTN_ENTER,
    BOARD_IN_BTN_LEFT,
    BOARD_IN_BTN_RIGHT,
    BOARD_IN_COUNT,
} board_input_t;

/** Called from ISR context: keep it short and ISR safe. */
typedef void (*board_input_isr_t)(board_input_t input, bool level, void *arg);

typedef enum {
    BOARD_PWM_TUBE = 0,
    BOARD_PWM_LCD,
    BOARD_PWM_BUZZER,
    BOARD_PWM_COUNT,
} board_pwm_t;

typedef struct {
    uint32_t freq_hz;
    float duty_pct;
} board_pwm_status_t;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */
esp_err_t board_init(void);

/** Pushes the persisted settings (charging, LED, backlight) to the hardware. */
void board_apply_settings(void);

/* Outputs */
void board_set_latch(bool on);
void board_set_charge_en(bool on);
void board_set_led(bool on);

/* Inputs */
bool board_input_level(board_input_t in);
esp_err_t board_input_set_isr(board_input_t in, board_input_isr_t cb, void *arg);
/** Free running tube pulse counter, incremented by the PIN_TUBE_CNT ISR. */
uint32_t board_tube_pulses(void);

/* PWM */
esp_err_t board_hv_set_duty(float duty_pct);
esp_err_t board_hv_set_freq(uint32_t freq_hz);
esp_err_t board_hv_set_enabled(bool enabled);
esp_err_t board_hv_regulator_start(void);
bool board_hv_is_enabled(void);
float board_hv_duty_pct(void);
float board_hv_output_duty_pct(void);
uint32_t board_hv_freq_hz(void);
esp_err_t board_pwm_get_status(board_pwm_t pwm, board_pwm_status_t *status);
esp_err_t board_backlight_set(uint8_t duty_pct);
esp_err_t board_buzzer_on(uint32_t freq_hz, uint8_t duty_level);
esp_err_t board_buzzer_off(void);

/* ADC, values are refreshed periodically in the background */
esp_err_t board_adc_get_raw(board_adc_ch_t ch, int *raw);
esp_err_t board_adc_get_mv(board_adc_ch_t ch, int *mv);
esp_err_t board_tube_voltage_get_mv(int *mv);

/* LCD, NULL when the display pins are set to -1 */
u8g2_t *board_lcd(void);

#ifdef __cplusplus
}
#endif
