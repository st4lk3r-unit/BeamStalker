/*
 * bs_keys_sic.c - Keystroke abstraction backed by SIC (hardware).
 *
 * Sources:
 *   - TCA8418 keyboard matrix via kscan_read_bitmap() + keymap function
 *   - Rotary encoder: ISR-driven on ESP32 (BS_ENC_PIN_A/B defined in board.h),
 *                     polled fallback on other platforms via SIC read_delta().
 *
 * Requires -DBS_KEYS_SIC and SIC configured for the target board.
 */
#ifdef BS_KEYS_SIC

#include "bs/bs_keys.h"
#include <sic/sic.h>
#include <sic/input/kscan.h>
#include <sic/input/encoder.h>

static unsigned long long s_prev_bitmap = 0;
static int  s_btn_prev = 0;

/* =========================================================================
 * ESP32 interrupt-driven encoder
 * When BS_ENC_PIN_A and BS_ENC_PIN_B are defined (in board variant header)
 * the encoder is read via GPIO interrupts instead of polling.  This makes
 * every state transition visible regardless of how long the main loop takes.
 * ========================================================================= */
#if defined(ARDUINO_ARCH_ESP32) && defined(BS_ENC_PIN_A) && defined(BS_ENC_PIN_B)

#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE     s_enc_mux   = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t s_enc_accum = 0;
static volatile int     s_enc_last  = 0;   /* previous (a<<1|b) state */

static const int8_t k_enc_isr_tbl[4][4] = {
    { 0, -1, +1,  0 },   /* prev 00 */
    {+1,  0,  0, -1 },   /* prev 01 */
    {-1,  0,  0, +1 },   /* prev 10 */
    { 0, +1, -1,  0 },   /* prev 11 */
};

static void IRAM_ATTR bs_enc_isr(void* arg) {
    (void)arg;
    int a   = gpio_get_level((gpio_num_t)BS_ENC_PIN_A);
    int b   = gpio_get_level((gpio_num_t)BS_ENC_PIN_B);
    int cur = (a << 1) | b;
    int d   = k_enc_isr_tbl[s_enc_last][cur];
    s_enc_last = cur;
    portENTER_CRITICAL_ISR(&s_enc_mux);
    s_enc_accum += d;
    portEXIT_CRITICAL_ISR(&s_enc_mux);
}

static void bs_enc_isr_init(void) {
    s_enc_last = (gpio_get_level((gpio_num_t)BS_ENC_PIN_A) << 1)
               |  gpio_get_level((gpio_num_t)BS_ENC_PIN_B);
    gpio_set_intr_type((gpio_num_t)BS_ENC_PIN_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type((gpio_num_t)BS_ENC_PIN_B, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add((gpio_num_t)BS_ENC_PIN_A, bs_enc_isr, NULL);
    gpio_isr_handler_add((gpio_num_t)BS_ENC_PIN_B, bs_enc_isr, NULL);
}

/* Returns +1 (CCW/up), -1 (CW/down), or 0. Consumes one detent at a time. */
static int bs_enc_read(void) {
    portENTER_CRITICAL(&s_enc_mux);
    int32_t acc = s_enc_accum;
    if (acc >= 4) {
        s_enc_accum -= 4;
        portEXIT_CRITICAL(&s_enc_mux);
        return +1;
    }
    if (acc <= -4) {
        s_enc_accum += 4;
        portEXIT_CRITICAL(&s_enc_mux);
        return -1;
    }
    portEXIT_CRITICAL(&s_enc_mux);
    return 0;
}

#define BS_ENC_ISR_AVAILABLE 1

#endif /* ARDUINO_ARCH_ESP32 && BS_ENC_PIN_A */

/* ---- Init ---------------------------------------------------------------- */

void bs_keys_init(const bs_arch_t* arch) {
    (void)arch;
    s_prev_bitmap = 0;
    s_btn_prev    = 0;
#ifdef BS_ENC_ISR_AVAILABLE
    bs_enc_isr_init();
#endif
}

/* ---- Poll ---------------------------------------------------------------- */

bool bs_keys_poll(bs_key_t* out) {
    if (!out) return false;
    out->ch = 0;

    /* ---- Rotary encoder ---- */
#ifdef BS_ENC_ISR_AVAILABLE
    {
        int delta = bs_enc_read();
        if (delta > 0) { out->id = BS_KEY_UP;   return true; }
        if (delta < 0) { out->id = BS_KEY_DOWN; return true; }
        /* Button still polled via SIC — button presses are slow/edge-detected */
        const encoder_t* enc = sic_encoder(0);
        if (enc) {
            int btn      = enc->v->read_btn(enc);
            int btn_edge = (btn == 1 && s_btn_prev == 0);
            s_btn_prev   = btn;
            if (btn_edge) { out->id = BS_KEY_ENTER; return true; }
        }
    }
#else
    {
        const encoder_t* enc = sic_encoder(0);
        if (enc) {
            int delta = enc->v->read_delta(enc);
            int btn   = enc->v->read_btn(enc);
            if (delta > 0) { out->id = BS_KEY_UP;   return true; }
            if (delta < 0) { out->id = BS_KEY_DOWN; return true; }
            int btn_edge = (btn == 1 && s_btn_prev == 0);
            s_btn_prev   = btn;
            if (btn_edge) { out->id = BS_KEY_ENTER; return true; }
        }
    }
#endif

    /* ---- Keyboard bitmap ---- */
    const kscan_t* kbd = sic_kbd(0);
    if (!kbd) return false;

    unsigned long long bitmap = 0;
    if (kscan_read_bitmap(kbd, &bitmap) < 0) return false;

    unsigned long long pressed = bitmap & ~s_prev_bitmap;  /* newly pressed */
    s_prev_bitmap = bitmap;
    if (!pressed) return false;

    bool alt   = (kbd->modifier_mask && (bitmap & kbd->modifier_mask));
    bool shift = (kbd->shift_mask    && (bitmap & kbd->shift_mask));

    for (int i = 0; i < 64; i++) {
        if (!(pressed & (1ULL << i))) continue;
        if (kbd->modifier_mask & (1ULL << i)) continue;
        if (kbd->shift_mask    & (1ULL << i)) continue;

        char ch = 0;
        if (alt && kbd->keymap_alt) {
            ch = kbd->keymap_alt(i);
        } else if (kbd->keymap) {
            ch = kbd->keymap(i);
        }
        if (!ch) continue;

        if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (ch == SIC_KEY_CAPS_LOCK) return false;

        switch (ch) {
            case '\n': case '\r': out->id = BS_KEY_ENTER; return true;
            case 0x1B:            out->id = BS_KEY_ESC;   return true;
            case 0x08:            out->id = BS_KEY_BACK;  return true;
            case '\x0F':          out->id = BS_KEY_FUNC;  return true;
        }

        if (ch >= 0x20 && ch < 0x7F) {
            out->id = BS_KEY_CHAR;
            out->ch = ch;
            return true;
        }
        break;
    }

    return false;
}

#endif /* BS_KEYS_SIC */
