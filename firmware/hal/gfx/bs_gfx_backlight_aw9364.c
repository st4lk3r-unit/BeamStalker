/*
 * bs_gfx_backlight_aw9364.c - AW9364 single-wire pulse-count backlight driver.
 *
 * Overrides the weak bs_gfx_backlight_hw() defined in bs_gfx_sgfx.c.
 * Compiled only when PORT_ARDUINO + SGFX_PIN_BL are defined (ESP32 target
 * with a backlight GPIO).
 *
 * AW9364 protocol:
 *   - EN low > 3 ms  → driver resets to maximum brightness (level 16)
 *   - Each rising edge on EN steps brightness down by one level
 *   - Level 16 = maximum, level 1 = minimum, EN held low = off
 *
 * To set level N:
 *   1. Pull EN low for ≥ 5 ms  (reset to level 16)
 *   2. Pulse EN high/low  (16 − N) times  (each pulse < 3 ms)
 *   3. Leave EN high  (backlight on at level N)
 */
#if defined(PORT_ARDUINO) && defined(SGFX_PIN_BL) && SGFX_PIN_BL >= 0

#include "driver/gpio.h"     /* gpio_set_direction, gpio_set_level */
#include "esp_rom_sys.h"     /* esp_rom_delay_us                    */

void bs_gfx_backlight_hw(int pct) {
    gpio_set_direction((gpio_num_t)SGFX_PIN_BL, GPIO_MODE_OUTPUT);

    if (pct == 0) {
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
        return;
    }

    /* Map 1–100 % → level 1–16 */
    int level = 1 + (pct - 1) * 15 / 99;
    if (level <  1) level =  1;
    if (level > 16) level = 16;

    /* Step 1: reset to max — EN low ≥ 5 ms */
    gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
    esp_rom_delay_us(5000);

    /* Step 2: pulse down from 16 to target level */
    int pulses = 16 - level;
    for (int i = 0; i < pulses; i++) {
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 1);
        esp_rom_delay_us(200);
        gpio_set_level((gpio_num_t)SGFX_PIN_BL, 0);
        esp_rom_delay_us(200);
    }

    /* Step 3: leave EN high — backlight active at level N */
    gpio_set_level((gpio_num_t)SGFX_PIN_BL, 1);
}

#endif /* PORT_ARDUINO && SGFX_PIN_BL */
