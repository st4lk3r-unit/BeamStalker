/*
 * bs_keys_gpio.c - Minimal GPIO button backend.
 *
 * Intended for tiny boards with only one button (e.g. LilyGO T-Dongle-S3).
 * Gesture mapping:
 *   short press  -> BS_KEY_RIGHT
 *   double press -> BS_KEY_BACK
 *   long press   -> BS_KEY_ENTER
 */
#ifdef BS_KEYS_GPIO

#include "bs/bs_keys.h"
#include "board.h"

#if defined(ARCH_ESP32)
#include "driver/gpio.h"
#include "esp_timer.h"

#ifndef BS_GPIOKEY_PIN
#  error "BS_KEYS_GPIO requires BS_GPIOKEY_PIN"
#endif
#ifndef BS_GPIOKEY_LONG_MS
#  define BS_GPIOKEY_LONG_MS 650
#endif
#ifndef BS_GPIOKEY_DOUBLE_MS
#  define BS_GPIOKEY_DOUBLE_MS 300
#endif

static int s_prev_level = 1;
static int s_pressed = 0;
static int s_long_fired = 0;
static int64_t s_press_ms = 0;
static int64_t s_release_ms = 0;
static int s_clicks = 0;

static inline int64_t bs_gpio_ms(void) {
    return esp_timer_get_time() / 1000;
}

static inline int bs_gpio_level(void) {
    return gpio_get_level((gpio_num_t)BS_GPIOKEY_PIN);
}

static inline int bs_gpio_is_pressed(int level) {
#if defined(BS_GPIOKEY_ACTIVE_LOW) && BS_GPIOKEY_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

void bs_keys_init(const bs_arch_t* arch) {
    (void)arch;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BS_GPIOKEY_PIN,
        .mode = GPIO_MODE_INPUT,
#if defined(BS_GPIOKEY_ACTIVE_LOW) && BS_GPIOKEY_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);

    s_prev_level  = bs_gpio_level();
    s_pressed     = bs_gpio_is_pressed(s_prev_level);
    s_long_fired  = 0;
    s_press_ms    = 0;
    s_release_ms  = 0;
    s_clicks      = 0;
}

bool bs_keys_poll(bs_key_t* out) {
    if (!out) return false;
    out->id = BS_KEY_NONE;
    out->ch = 0;

    int64_t now   = bs_gpio_ms();
    int level     = bs_gpio_level();
    int pressed   = bs_gpio_is_pressed(level);

    if (pressed && !s_pressed) {
        s_pressed = 1;
        s_long_fired = 0;
        s_press_ms = now;
    } else if (!pressed && s_pressed) {
        s_pressed = 0;
        if (!s_long_fired) {
            s_clicks++;
            s_release_ms = now;
        }
    }
    s_prev_level = level;

    if (s_pressed && !s_long_fired && (now - s_press_ms) >= BS_GPIOKEY_LONG_MS) {
        s_long_fired = 1;
        s_clicks = 0;
        out->id = BS_KEY_ENTER;
        return true;
    }

    if (!s_pressed && s_clicks > 0 && (now - s_release_ms) >= BS_GPIOKEY_DOUBLE_MS) {
        if (s_clicks >= 2) {
            s_clicks = 0;
            out->id = BS_KEY_BACK;
            return true;
        }
        s_clicks = 0;
        out->id = BS_KEY_RIGHT;
        return true;
    }

    return false;
}

#else
#error "BS_KEYS_GPIO currently requires ARCH_ESP32"
#endif

#endif /* BS_KEYS_GPIO */
