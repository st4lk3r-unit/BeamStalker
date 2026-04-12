#pragma once
/*
 * ble_spam_svc.h - BLE spam service layer.
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include <stdint.h>
#include "bs/bs_arch.h"
#include "apps/ble/ble_common.h"

typedef enum {
    BLE_SPAM_SVC_IDLE = 0,
    BLE_SPAM_SVC_RUNNING,
} ble_spam_svc_state_t;

void ble_spam_svc_init(const bs_arch_t* arch);
void ble_spam_svc_start(ble_spam_mode_t mode, uint32_t interval_ms);
void ble_spam_svc_stop(void);
void ble_spam_svc_tick(uint32_t now_ms);

ble_spam_svc_state_t ble_spam_svc_state(void);
ble_spam_mode_t      ble_spam_svc_mode(void);
uint32_t             ble_spam_svc_interval_ms(void);
uint32_t             ble_spam_svc_count(void);
void                 ble_spam_svc_set_mode(ble_spam_mode_t mode);
void                 ble_spam_svc_set_interval(uint32_t ms);

#endif /* BS_HAS_BLE */
