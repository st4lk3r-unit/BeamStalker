/*
 * wifi_beacon_svc.c - Beacon spam service (state machine, no UI).
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_beacon_svc.h"
#include "wifi_common.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"

#include <string.h>
#include <stdio.h>

/* ── Config ─────────────────────────────────────────────────────────────── */

#define BEACON_BUF_SIZE   128
#define SSID_MAX_LEN       33
#define BURST_INTERVAL_MS   8

static const int k_repeat_vals[] = {1, 2, 3, 5, 10};

/* ── Internal state ──────────────────────────────────────────────────────── */

static const bs_arch_t*   s_arch;
static beacon_svc_state_t s_state;
static beacon_svc_mode_t  s_mode;
static wifi_charset_t     s_charset;
static char               s_prefix[21];
static int                s_repeat;

/* File SSIDs — pointer to caller-owned array */
static const char (*s_file_ssids)[SSID_MAX_LEN];
static int         s_file_ssid_count;
static int         s_file_ssid_idx;

/* Running */
static wifi_pps_t  s_pps;
static wifi_prng_t s_prng;
static uint8_t     s_bssid[6];
static uint8_t     s_frame[BEACON_BUF_SIZE];
static char        s_cur_ssid[SSID_MAX_LEN];
static int         s_burst_left;
static uint32_t    s_last_tx_ms;
static int         s_cur_flen;
static uint8_t     s_cur_ch;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void next_ssid(void) {
    switch (s_mode) {
        case BEACON_MODE_FILE:
            if (s_file_ssid_count > 0) {
                strncpy(s_cur_ssid, s_file_ssids[s_file_ssid_idx], SSID_MAX_LEN - 1);
                s_cur_ssid[SSID_MAX_LEN - 1] = '\0';
                s_file_ssid_idx = (s_file_ssid_idx + 1) % s_file_ssid_count;
                break;
            }
            /* fallthrough */
        case BEACON_MODE_RANDOM:
            wifi_random_ssid_charset(&s_prng, s_charset, s_cur_ssid, SSID_MAX_LEN);
            break;
        case BEACON_MODE_CUSTOM: {
            int num = (int)(wifi_prng_next(&s_prng) % 1000);
            snprintf(s_cur_ssid, SSID_MAX_LEN, "%s%03d", s_prefix, num);
            break;
        }
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void beacon_svc_init(const bs_arch_t* arch) {
    s_arch  = arch;
    s_state = BEACON_SVC_IDLE;
}

void beacon_svc_start(beacon_svc_mode_t mode, wifi_charset_t charset,
                      const char* prefix, int repeat,
                      const char (*file_ssids)[33], int file_ssid_count) {
    s_mode            = mode;
    s_charset         = charset;
    s_repeat          = repeat;
    s_file_ssids      = file_ssids;
    s_file_ssid_count = file_ssid_count;
    s_file_ssid_idx   = 0;

    if (prefix && prefix[0])
        strncpy(s_prefix, prefix, sizeof(s_prefix) - 1);
    else
        strncpy(s_prefix, "testAP", sizeof(s_prefix) - 1);
    s_prefix[sizeof(s_prefix) - 1] = '\0';

    bs_wifi_set_tx_power(20);
    wifi_pps_init(&s_pps);
    wifi_prng_seed(&s_prng, s_arch->millis() ^ 0xBEEFCAFEu);
    s_burst_left  = 0;
    s_last_tx_ms  = 0;
    s_cur_flen    = 0;
    s_cur_ch      = 1;
    s_state       = BEACON_SVC_RUNNING;
}

void beacon_svc_stop(void) {
    s_state = BEACON_SVC_IDLE;
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

void beacon_svc_tick(uint32_t now_ms) {
    if (s_state != BEACON_SVC_RUNNING) return;

    if (s_burst_left <= 0) {
        wifi_random_mac(&s_prng, s_bssid);
        s_cur_ch = (uint8_t)(wifi_prng_next(&s_prng) % 13) + 1;
        bs_wifi_set_channel(s_cur_ch);
        next_ssid();
        s_cur_flen   = wifi_build_beacon(s_frame, sizeof s_frame,
                                         s_cur_ssid, s_bssid, s_cur_ch);
        s_burst_left = s_repeat;
    }

    if (s_burst_left > 0 && (now_ms - s_last_tx_ms) >= BURST_INTERVAL_MS) {
        if (s_cur_flen > 0) {
            int err = bs_wifi_send_raw(BS_WIFI_IF_STA, s_frame, (uint16_t)s_cur_flen);
            if (err == 0) wifi_pps_tick(&s_pps, now_ms);
        }
        s_burst_left--;
        s_last_tx_ms = now_ms;
    }
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

beacon_svc_state_t beacon_svc_state(void)     { return s_state;      }
uint32_t           beacon_svc_frames(void)     { return s_pps.total;  }
uint32_t           beacon_svc_pps(void)        { return s_pps.pps;    }
const char*        beacon_svc_cur_ssid(void)   { return s_cur_ssid;   }
uint8_t            beacon_svc_cur_channel(void){ return s_cur_ch;     }
const uint8_t*     beacon_svc_cur_bssid(void)  { return s_bssid;      }
int                beacon_svc_burst_left(void) { return s_burst_left; }

#endif /* BS_HAS_WIFI */
