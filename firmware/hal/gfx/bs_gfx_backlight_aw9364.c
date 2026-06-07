/*
 * bs_gfx_backlight_aw9364.c - board-specific LCD backlight control.
 *
 * Historical note:
 *   This file used to assume every SGFX_PIN_BL board had an AW9364-style
 *   single-wire pulse-count LED driver.  That is only true for boards that
 *   explicitly select BS_BACKLIGHT_AW9364.  Cardputer/Cardputer-ADV use GPIO38
 *   as the LCD backlight gate/PWM input; on Stamp-S3A boards the same rail also
 *   powers the RGB LED, so AW9364 pulse trains on GPIO38 can make the RGB LED
 *   flicker or latch a random colour.
 */
#if defined(PORT_ARDUINO) && defined(SGFX_PIN_BL) && SGFX_PIN_BL >= 0

#include "board.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef BS_SGFX_BL_ACTIVE_LOW
#  define BS_SGFX_BL_ACTIVE_LOW 0
#endif

static inline int bl_level_for_on(void)  { return BS_SGFX_BL_ACTIVE_LOW ? 0 : 1; }
static inline int bl_level_for_off(void) { return BS_SGFX_BL_ACTIVE_LOW ? 1 : 0; }

#if defined(BS_BACKLIGHT_AW9364)

/*
 * AW9364 protocol:
 *   - EN low > 3 ms  -> reset to maximum brightness (level 16)
 *   - Each rising edge steps brightness down by one level
 *   - Leave EN high to keep the selected level enabled
 */
void bs_gfx_backlight_hw(int pct) {
    gpio_set_direction((gpio_num_t)SGFX_PIN_BL, GPIO_MODE_OUTPUT);

    if (pct <= 0) {
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
        return;
    }
    if (pct > 100) pct = 100;

    /* Map 1-100 % -> level 1-16 */
    int level = 1 + (pct - 1) * 15 / 99;
    if (level <  1) level =  1;
    if (level > 16) level = 16;

    gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
    esp_rom_delay_us(5000);  /* reset to level 16 */

    int pulses = 16 - level;
    for (int i = 0; i < pulses; i++) {
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 1);
        esp_rom_delay_us(200);
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
        esp_rom_delay_us(200);
    }

    gpio_set_level((gpio_num_t)SGFX_PIN_BL, 1);
}

#elif defined(BS_BACKLIGHT_GPIO_ONLY)

/* Reliable on/off gate only.  Do not PWM this pin on Cardputer-class boards. */
void bs_gfx_backlight_hw(int pct) {
    gpio_set_direction((gpio_num_t)SGFX_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SGFX_PIN_BL, pct > 0 ? bl_level_for_on() : bl_level_for_off());
}

#elif defined(BS_BACKLIGHT_PWM)

#include "driver/ledc.h"

#ifndef BS_BACKLIGHT_PWM_FREQ_HZ
#  define BS_BACKLIGHT_PWM_FREQ_HZ 5000
#endif
#ifndef BS_BACKLIGHT_PWM_BITS
#  define BS_BACKLIGHT_PWM_BITS LEDC_TIMER_10_BIT
#endif
#ifndef BS_BACKLIGHT_PWM_TIMER
#  define BS_BACKLIGHT_PWM_TIMER LEDC_TIMER_3
#endif
#ifndef BS_BACKLIGHT_PWM_CHANNEL
#  define BS_BACKLIGHT_PWM_CHANNEL LEDC_CHANNEL_7
#endif

static bool s_pwm_ready = false;

static void bl_pwm_init(void) {
    if (s_pwm_ready) return;

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BS_BACKLIGHT_PWM_BITS,
        .timer_num       = BS_BACKLIGHT_PWM_TIMER,
        .freq_hz         = BS_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    (void)ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = SGFX_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BS_BACKLIGHT_PWM_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BS_BACKLIGHT_PWM_TIMER,
        .duty       = BS_SGFX_BL_ACTIVE_LOW ? 1023 : 0,
        .hpoint     = 0,
    };
    (void)ledc_channel_config(&ch);
    s_pwm_ready = true;
}

void bs_gfx_backlight_hw(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    bl_pwm_init();

    const uint32_t max_duty = 1023U;
    uint32_t duty = ((uint32_t)pct * max_duty) / 100U;
    if (BS_SGFX_BL_ACTIVE_LOW) duty = max_duty - duty;

    (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, BS_BACKLIGHT_PWM_CHANNEL, duty);
    (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, BS_BACKLIGHT_PWM_CHANNEL);
}

#else

/* Safe fallback: boards without an explicit dimming mode get on/off only. */
void bs_gfx_backlight_hw(int pct) {
    gpio_set_direction((gpio_num_t)SGFX_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SGFX_PIN_BL, pct > 0 ? bl_level_for_on() : bl_level_for_off());
}

#endif

#endif /* PORT_ARDUINO && SGFX_PIN_BL */
