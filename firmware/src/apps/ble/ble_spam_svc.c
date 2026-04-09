/*
 * ble_spam_svc.c - BLE spam service (state machine, no UI).
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "ble_spam_svc.h"
#include "ble_common.h"
#include "bs/bs_ble.h"
#include "bs/bs_arch.h"

/* ── Internal state ──────────────────────────────────────────────────────── */

static const bs_arch_t*   s_arch;
static ble_spam_svc_state_t s_state;
static ble_spam_mode_t    s_mode;
static uint32_t           s_interval_ms;
static uint32_t           s_count;
static uint32_t           s_last_ms;
static uint32_t           s_rng;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void ble_spam_svc_init(const bs_arch_t* arch) {
    s_arch  = arch;
    s_state = BLE_SPAM_SVC_IDLE;
}

void ble_spam_svc_start(ble_spam_mode_t mode, uint32_t interval_ms) {
    s_mode        = mode;
    s_interval_ms = interval_ms > 0 ? interval_ms : 100;
    s_count       = 0;
    s_last_ms     = s_arch->millis();
    s_rng         = s_last_ms ^ 0xBEEF1234u;
    bs_ble_set_tx_power(9);
    s_state = BLE_SPAM_SVC_RUNNING;
}

void ble_spam_svc_stop(void) {
    if (s_state == BLE_SPAM_SVC_RUNNING)
        bs_ble_adv_stop();
    s_state = BLE_SPAM_SVC_IDLE;
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

void ble_spam_svc_tick(uint32_t now_ms) {
    if (s_state != BLE_SPAM_SVC_RUNNING) return;
    if ((now_ms - s_last_ms) < s_interval_ms) return;
    s_last_ms = now_ms;

    bs_ble_adv_stop();

    uint8_t addr[6];
    ble_random_addr(addr, &s_rng);
    bs_ble_set_addr(addr);

    bs_ble_adv_data_t payload;
    ble_build_payload(&payload, s_mode, &s_rng);
    bs_ble_adv_start(&payload, s_interval_ms);

    s_count++;
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

ble_spam_svc_state_t ble_spam_svc_state(void)       { return s_state;       }
ble_spam_mode_t      ble_spam_svc_mode(void)         { return s_mode;        }
uint32_t             ble_spam_svc_interval_ms(void)  { return s_interval_ms; }
uint32_t             ble_spam_svc_count(void)        { return s_count;       }

void ble_spam_svc_set_mode(ble_spam_mode_t mode) {
    s_mode = mode;
}

void ble_spam_svc_set_interval(uint32_t ms) {
    if (ms > 0) s_interval_ms = ms;
}

#endif /* BS_HAS_BLE */
