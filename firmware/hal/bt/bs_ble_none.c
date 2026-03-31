/*
 * bs_ble_none.c - No-op BLE backend.
 *
 * Activated when no real BLE backend is selected.
 * bs_ble_caps() returns 0; all other calls return -1.
 */
#if !defined(BS_BLE_ESP32) && !defined(BS_BLE_NRF)

#include "bs/bs_ble.h"

int      bs_ble_init(const bs_arch_t* a)             { (void)a; return 0;           }
void     bs_ble_deinit(void)                         {                               }
uint32_t bs_ble_caps(void)                           { return 0;                    }

int  bs_ble_set_addr(const uint8_t a[6])             { (void)a; return -1;          }
int  bs_ble_set_tx_power(int dbm)                    { (void)dbm; return -1;        }

int  bs_ble_adv_start(const bs_ble_adv_data_t* d, uint32_t ms)
     { (void)d; (void)ms; return -1; }
void bs_ble_adv_stop(void)                           {                               }
int  bs_ble_adv_update(const bs_ble_adv_data_t* d)  { (void)d; return -1;          }

int  bs_ble_scan_start(bs_ble_scan_cb_t cb, void* ctx)
     { (void)cb; (void)ctx; return -1; }
void bs_ble_scan_stop(void)                          {                               }

#endif /* no backend */
