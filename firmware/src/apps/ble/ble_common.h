#pragma once
/*
 * ble_common.h - Shared BLE utilities: PRNG, random MAC, vendor detection.
 */
#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_ble.h"

/* ── LCG PRNG ───────────────────────────────────────────────────────────── */

static inline void ble_prng_seed(uint32_t* state, uint32_t seed) {
    *state = seed ? seed : 0xDEADBEEFu;
}

static inline uint32_t ble_lcg_next(uint32_t* state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

/* ── Random MAC (random static address: top 2 bits of byte[5] set) ──────── */

static inline void ble_random_addr(uint8_t addr[6], uint32_t* rng) {
    for (int i = 0; i < 6; i++)
        addr[i] = (uint8_t)(ble_lcg_next(rng) & 0xFF);
    addr[0] |= 0xC0;  /* random static: ESP32 treats addr[0] as MSB; bits 7+6 must be 11 */
}

/* ── Vendor detection from raw AD bytes ─────────────────────────────────── */

typedef enum {
    BLE_VENDOR_UNKNOWN   = 0,
    BLE_VENDOR_APPLE,
    BLE_VENDOR_SAMSUNG,
    BLE_VENDOR_GOOGLE,
    BLE_VENDOR_MICROSOFT,
} ble_vendor_t;

static inline const char* ble_vendor_str(ble_vendor_t v) {
    switch (v) {
        case BLE_VENDOR_APPLE:     return "Apple";
        case BLE_VENDOR_SAMSUNG:   return "Samsung";
        case BLE_VENDOR_GOOGLE:    return "Google";
        case BLE_VENDOR_MICROSOFT: return "Microsoft";
        default:                   return "???";
    }
}

/*
 * Walk the AD records in data[0..len-1] and return the first matched vendor.
 * Checks: type 0xFF (manufacturer-specific) for Apple/Samsung/Microsoft;
 *         type 0x03/0x16 (16-bit UUID) for Google Fast Pair (0xFE2C).
 */
static inline ble_vendor_t ble_detect_vendor(const uint8_t* data, uint8_t len) {
    uint8_t i = 0;
    while (i < len) {
        uint8_t ad_len = data[i];
        if (ad_len == 0 || (i + ad_len) >= len) break;
        uint8_t ad_type        = data[i + 1];
        const uint8_t* ad_data = data + i + 2;
        uint8_t ad_dlen        = ad_len - 1;

        if (ad_type == 0xFF && ad_dlen >= 2) {
            uint16_t mfr = (uint16_t)(ad_data[0] | ((uint16_t)ad_data[1] << 8));
            if (mfr == 0x004C) return BLE_VENDOR_APPLE;
            if (mfr == 0x0075) return BLE_VENDOR_SAMSUNG;
            if (mfr == 0x0006) return BLE_VENDOR_MICROSOFT;
        }
        if ((ad_type == 0x03 || ad_type == 0x16) && ad_dlen >= 2) {
            uint16_t uuid = (uint16_t)(ad_data[0] | ((uint16_t)ad_data[1] << 8));
            if (uuid == 0xFE2C) return BLE_VENDOR_GOOGLE;
        }
        i += ad_len + 1;
    }
    return BLE_VENDOR_UNKNOWN;
}
