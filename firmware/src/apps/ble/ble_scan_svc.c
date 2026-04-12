/*
 * ble_scan_svc.c - BLE passive scanner service (no UI).
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "ble_scan_svc.h"
#include "ble_common.h"
#include "bs/bs_ble.h"
#include "bs/bs_arch.h"

#include <string.h>

/* ── Internal state ──────────────────────────────────────────────────────── */

static ble_scan_svc_state_t s_state;
static ble_scan_dev_t       s_devs[BLE_SCAN_SVC_MAX_DEVS];
static int                  s_count;
static bool                 s_dirty;

/* ── BLE callback (BLE stack task) ──────────────────────────────────────── */

static void scan_cb(const bs_ble_scan_result_t* r, void* ctx) {
    (void)ctx;
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_devs[i].addr, r->addr, 6) == 0) {
            s_devs[i].rssi = r->rssi;
            s_dirty = true;
            return;
        }
    }
    if (s_count >= BLE_SCAN_SVC_MAX_DEVS) return;
    int idx = s_count++;
    memcpy(s_devs[idx].addr, r->addr, 6);
    s_devs[idx].rssi   = r->rssi;
    s_devs[idx].vendor = ble_detect_vendor(r->adv.data, r->adv.len);
    s_dirty = true;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void ble_scan_svc_init(const bs_arch_t* arch) {
    (void)arch;
    s_state = BLE_SCAN_SVC_IDLE;
}

void ble_scan_svc_start(void) {
    memset(s_devs, 0, sizeof(s_devs));
    s_count = 0;
    s_dirty = false;
    bs_ble_scan_start(scan_cb, NULL);
    s_state = BLE_SCAN_SVC_RUNNING;
}

void ble_scan_svc_stop(void) {
    if (s_state == BLE_SCAN_SVC_RUNNING)
        bs_ble_scan_stop();
    s_state = BLE_SCAN_SVC_IDLE;
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

ble_scan_svc_state_t ble_scan_svc_state(void) { return s_state; }
int                  ble_scan_svc_count(void)  { return s_count; }
bool                 ble_scan_svc_dirty(void)  { return s_dirty; }
void ble_scan_svc_clear_dirty(void) { s_dirty = false; }

const ble_scan_dev_t* ble_scan_svc_dev(int idx) {
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_devs[idx];
}

#endif /* BS_HAS_BLE */
