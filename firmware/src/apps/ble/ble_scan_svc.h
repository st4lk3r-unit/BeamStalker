#pragma once
/*
 * ble_scan_svc.h - BLE passive scanner service layer.
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"
#include "apps/ble/ble_common.h"

#define BLE_SCAN_SVC_MAX_DEVS 64

typedef struct {
    uint8_t      addr[6];
    int8_t       rssi;
    ble_vendor_t vendor;
} ble_scan_dev_t;

typedef enum {
    BLE_SCAN_SVC_IDLE = 0,
    BLE_SCAN_SVC_RUNNING,
} ble_scan_svc_state_t;

void ble_scan_svc_init(const bs_arch_t* arch);
void ble_scan_svc_start(void);
void ble_scan_svc_stop(void);

ble_scan_svc_state_t    ble_scan_svc_state(void);
int                     ble_scan_svc_count(void);
const ble_scan_dev_t*   ble_scan_svc_dev(int idx);
bool                    ble_scan_svc_dirty(void);
void                    ble_scan_svc_clear_dirty(void);

#endif /* BS_HAS_BLE */
