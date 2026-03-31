#pragma once
/*
 * bs_ble.h - BeamStalker Bluetooth Low Energy abstraction.
 *
 * Backends (select exactly one via build flags):
 *   BS_BLE_ESP32   - ESP32/S3 built-in BLE (ADVERTISE + SCAN + RAND_ADDR)
 *   BS_BLE_NRF     - nRF52/nRF5340 via Zephyr BT API (ADVERTISE + SCAN)
 *   (none defined) - silent no-ops; bs_ble_caps() returns 0
 *
 * Design:
 *   The API exposes raw advertisement payloads (byte arrays) rather than
 *   higher-level service UUIDs.  This enables full control over what is
 *   advertised — including spoofed vendor-specific payloads matching Apple,
 *   Samsung, Google, and other proprietary BLE protocols.
 *
 *   Advertisement data is a standard 31-byte BLE AD structure.  Apps build
 *   the payload and hand it to bs_ble_adv_start(); the backend transmits it
 *   as-is with no modification.
 *
 * Thread safety: not guaranteed. Call from one task/thread only.
 * Callbacks fire from the BLE stack task — keep them short.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bs_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capability flags ──────────────────────────────────────────────────── */

#define BS_BLE_CAP_ADVERTISE  (1u << 0)  /* send BLE advertisements        */
#define BS_BLE_CAP_SCAN       (1u << 1)  /* passive / active BLE scan      */
#define BS_BLE_CAP_RAND_ADDR  (1u << 2)  /* set a random 48-bit address    */

/* ── Advertisement payload ─────────────────────────────────────────────── */

/* Standard BLE AD structure: up to 31 bytes of AD records.                */
typedef struct {
    uint8_t data[31];
    uint8_t len;        /* number of valid bytes in data[] */
} bs_ble_adv_data_t;

/* ── Scan result ────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t          addr[6];
    int8_t           rssi;
    bs_ble_adv_data_t adv;
} bs_ble_scan_result_t;

/* Called for each received advertisement during a scan.
 * Fires from the BLE stack task — do not call bs_ble_* from inside.       */
typedef void (*bs_ble_scan_cb_t)(const bs_ble_scan_result_t* result, void* ctx);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Initialise BLE controller and stack.  Must be called before any other
 * bs_ble_* function.  Idempotent.
 * Returns 0 on success, <0 on hardware error.                             */
int  bs_ble_init(const bs_arch_t* arch);

/* Shut down BLE, free controller resources.                               */
void bs_ble_deinit(void);

/* Bitmask of BS_BLE_CAP_* flags supported by this backend + hardware.    */
uint32_t bs_ble_caps(void);

/* ── Address ────────────────────────────────────────────────────────────── */

/* Set a random 48-bit BLE device address.
 * The address type follows BT spec (random static / NRPA as set by caller).
 * Returns 0 on success, -1 if unsupported, -2 on driver error.           */
int bs_ble_set_addr(const uint8_t addr[6]);

/* ── TX power ───────────────────────────────────────────────────────────── */

/* Set BLE transmit power in dBm.  Clamped to hardware limits.
 * ESP32 typical range: -12 to +9 dBm.
 * Returns 0 on success, <0 if unsupported.                                */
int bs_ble_set_tx_power(int dbm);

/* ── Advertising ────────────────────────────────────────────────────────── */

/* Begin advertising with the given payload.
 *   data         - AD structure bytes (31-byte max).
 *   interval_ms  - advertisement interval in milliseconds (typ. 20–10240).
 *                  Smaller = more visible, higher power consumption.
 * Returns 0 on success, -1 if unsupported, -2 on driver error.           */
int  bs_ble_adv_start(const bs_ble_adv_data_t* data, uint32_t interval_ms);

/* Stop advertising.                                                        */
void bs_ble_adv_stop(void);

/* Update the advertisement payload without stopping.  Useful for cycling
 * through different spoofed device profiles.
 * Returns 0 on success, <0 on error.                                      */
int  bs_ble_adv_update(const bs_ble_adv_data_t* data);

/* ── Scanning ───────────────────────────────────────────────────────────── */

/* Start passive BLE scan.  cb is called for every received advertisement.
 * Returns 0 on success, -1 if unsupported, -2 on driver error.           */
int  bs_ble_scan_start(bs_ble_scan_cb_t cb, void* ctx);
void bs_ble_scan_stop(void);

#ifdef __cplusplus
}
#endif
