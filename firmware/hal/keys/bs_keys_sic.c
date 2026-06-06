/*
 * bs_keys_sic.c - BeamStalker keystroke abstraction backed by SIC.
 *
 * This file deliberately consumes SIC's high-level sic_key_poll() event layer.
 * Do not read raw kscan bitmaps here: board-specific matrix transforms,
 * modifier/caps/Fn logic, and Cardputer-ADV TCA8418 normalization belong in
 * SIC, not in BeamStalker.
 */
#ifdef BS_KEYS_SIC

#include "bs/bs_keys.h"
#include <sic/sic.h>
#include <sic/input/encoder.h>

static int  s_btn_prev = 0;
static int32_t s_enc_poll_pending = 0;

/* =========================================================================
 * ESP32 interrupt-driven encoder
 * When BS_ENC_PIN_A and BS_ENC_PIN_B are defined (in board variant header)
 * the encoder is read via GPIO interrupts instead of polling. This makes
 * every state transition visible regardless of how long the main loop takes.
 * ========================================================================= */
#if defined(ARDUINO_ARCH_ESP32) && defined(BS_ENC_PIN_A) && defined(BS_ENC_PIN_B)

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE     s_enc_mux      = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t s_enc_accum    = 0;
static volatile int32_t s_enc_pending  = 0;
static volatile int     s_enc_last     = 0;   /* previous (a<<1|b) state */
static bool             s_enc_isr_live = false;

#if defined(VARIANT_TPAGER)
#define BS_ENC_ISR_DETENT_STEPS 2
#define BS_ENC_ISR_DETENT_MASK  ((1u << 0) | (1u << 3))   /* half-step detents: 00 + 11 */
#else
#define BS_ENC_ISR_DETENT_STEPS 4
#define BS_ENC_ISR_DETENT_MASK  (1u << 0)                 /* full-step detent: 00 */
#endif

static const int8_t k_enc_isr_tbl[4][4] = {
    { 0, -1, +1,  0 },   /* prev 00 */
    {+1,  0,  0, -1 },   /* prev 01 */
    {-1,  0,  0, +1 },   /* prev 10 */
    { 0, +1, -1,  0 },   /* prev 11 */
};

static inline bool IRAM_ATTR bs_enc_is_detent_state(int st) {
    return (BS_ENC_ISR_DETENT_MASK & (1u << st)) != 0;
}

static void IRAM_ATTR bs_enc_isr(void* arg) {
    (void)arg;
    int a   = gpio_get_level((gpio_num_t)BS_ENC_PIN_A);
    int b   = gpio_get_level((gpio_num_t)BS_ENC_PIN_B);
    int cur = (a << 1) | b;
    int d   = k_enc_isr_tbl[s_enc_last][cur];
    s_enc_last = cur;
    if (!d) return;

    portENTER_CRITICAL_ISR(&s_enc_mux);
    s_enc_accum += d;

    if (bs_enc_is_detent_state(cur)) {
        if (s_enc_accum >= BS_ENC_ISR_DETENT_STEPS) {
            s_enc_pending += 1;
            s_enc_accum = 0;
        } else if (s_enc_accum <= -BS_ENC_ISR_DETENT_STEPS) {
            s_enc_pending -= 1;
            s_enc_accum = 0;
        } else if (s_enc_accum > -BS_ENC_ISR_DETENT_STEPS &&
                   s_enc_accum <  BS_ENC_ISR_DETENT_STEPS) {
            s_enc_accum = 0;
        }
    }

    portEXIT_CRITICAL_ISR(&s_enc_mux);
}

static bool bs_enc_isr_init(void) {
    s_enc_last = (gpio_get_level((gpio_num_t)BS_ENC_PIN_A) << 1)
               |  gpio_get_level((gpio_num_t)BS_ENC_PIN_B);

    esp_err_t rc = gpio_install_isr_service(0);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE)
        return false;

    if (gpio_set_intr_type((gpio_num_t)BS_ENC_PIN_A, GPIO_INTR_ANYEDGE) != ESP_OK)
        return false;
    if (gpio_set_intr_type((gpio_num_t)BS_ENC_PIN_B, GPIO_INTR_ANYEDGE) != ESP_OK)
        return false;

    gpio_isr_handler_remove((gpio_num_t)BS_ENC_PIN_A);
    gpio_isr_handler_remove((gpio_num_t)BS_ENC_PIN_B);

    if (gpio_isr_handler_add((gpio_num_t)BS_ENC_PIN_A, bs_enc_isr, NULL) != ESP_OK)
        return false;
    if (gpio_isr_handler_add((gpio_num_t)BS_ENC_PIN_B, bs_enc_isr, NULL) != ESP_OK)
        return false;

    return true;
}

/* Returns +1 (CCW/up), -1 (CW/down), or 0. Consumes one detent at a time. */
static int bs_enc_read(void) {
    portENTER_CRITICAL(&s_enc_mux);
    int32_t pending = s_enc_pending;
    if (pending > 0) {
        s_enc_pending = pending - 1;
        portEXIT_CRITICAL(&s_enc_mux);
        return +1;
    }
    if (pending < 0) {
        s_enc_pending = pending + 1;
        portEXIT_CRITICAL(&s_enc_mux);
        return -1;
    }
    portEXIT_CRITICAL(&s_enc_mux);
    return 0;
}

#define BS_ENC_ISR_AVAILABLE 1

#endif /* ARDUINO_ARCH_ESP32 && BS_ENC_PIN_A */

static bool bs_poll_encoder(bs_key_t* out) {
#ifdef BS_ENC_ISR_AVAILABLE
    if (s_enc_isr_live) {
        int delta = bs_enc_read();
        if (delta > 0) { out->id = BS_KEY_UP;   return true; }
        if (delta < 0) { out->id = BS_KEY_DOWN; return true; }

        const encoder_t* enc = sic_encoder(0);
        if (enc && enc->v && enc->v->read_btn) {
            int btn      = enc->v->read_btn(enc);
            int btn_edge = (btn == 1 && s_btn_prev == 0);
            s_btn_prev   = btn;
            if (btn_edge) { out->id = BS_KEY_ENTER; return true; }
        }
        return false;
    }
#endif

    const encoder_t* enc = sic_encoder(0);
    if (!enc || !enc->v) return false;

    if (enc->v->read_delta) {
        int delta = enc->v->read_delta(enc);
        if (delta != 0) {
            s_enc_poll_pending += delta;
            if (s_enc_poll_pending > 64) s_enc_poll_pending = 64;
            if (s_enc_poll_pending < -64) s_enc_poll_pending = -64;
        }
    }

    if (s_enc_poll_pending > 0) {
        s_enc_poll_pending--;
        out->id = BS_KEY_UP;
        return true;
    }
    if (s_enc_poll_pending < 0) {
        s_enc_poll_pending++;
        out->id = BS_KEY_DOWN;
        return true;
    }

    if (enc->v->read_btn) {
        int btn      = enc->v->read_btn(enc);
        int btn_edge = (btn == 1 && s_btn_prev == 0);
        s_btn_prev   = btn;
        if (btn_edge) { out->id = BS_KEY_ENTER; return true; }
    }

    return false;
}

static bool bs_map_sic_key(const sic_key_event_t* ev, bs_key_t* out) {
    if (!ev || !out || !ev->pressed) return false;

    switch (ev->code) {
        case SIC_KEY_ENTER:     out->id = BS_KEY_ENTER; return true;
        case SIC_KEY_BACKSPACE: out->id = BS_KEY_BACK;  return true;
        case SIC_KEY_ESC:       out->id = BS_KEY_ESC;   return true;
        case SIC_KEY_UP:        out->id = BS_KEY_UP;    return true;
        case SIC_KEY_DOWN:      out->id = BS_KEY_DOWN;  return true;
        case SIC_KEY_LEFT:      out->id = BS_KEY_LEFT;  return true;
        case SIC_KEY_RIGHT:     out->id = BS_KEY_RIGHT; return true;
        case SIC_KEY_DEL:       out->id = BS_KEY_BACK;  return true;
        case SIC_KEY_FN:
        case SIC_KEY_ALT:
        case SIC_KEY_OPT:       out->id = BS_KEY_FUNC;  return true;
        default: break;
    }

    if (ev->ascii >= 0x20 && ev->ascii < 0x7F) {
        out->id = BS_KEY_CHAR;
        out->ch = ev->ascii;
        return true;
    }

    return false;
}

void bs_keys_init(const bs_arch_t* arch) {
    (void)arch;
    s_btn_prev = 0;
    s_enc_poll_pending = 0;
#ifdef BS_ENC_ISR_AVAILABLE
    s_enc_isr_live = bs_enc_isr_init();
#endif
}

bool bs_keys_poll(bs_key_t* out) {
    if (!out) return false;
    out->id = BS_KEY_NONE;
    out->ch = 0;

    if (bs_poll_encoder(out)) return true;

    sic_key_event_t ev;
    while (sic_key_poll(&ev) > 0) {
        if (bs_map_sic_key(&ev, out)) return true;
    }

    return false;
}

#endif /* BS_KEYS_SIC */
