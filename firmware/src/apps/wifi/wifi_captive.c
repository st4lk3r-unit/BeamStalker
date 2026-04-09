/*
 * wifi_captive.c - Manual captive portal sub-application.
 *
 * Flow:
 *   SSID_EDIT   - type a custom SSID using character cycling
 *                   UP/DOWN    = cycle character at cursor
 *                   LEFT/RIGHT = move cursor
 *                   SELECT     = go to channel select
 *                   BACK       = exit
 *   CH_SELECT   - pick AP channel (1-13)
 *                   UP/DOWN    = increment/decrement channel
 *                   SELECT     = launch portal
 *                   BACK       = back to SSID edit
 *   RUNNING     - open SoftAP; DNS catch-all on :53; HTTP on :80
 *                   Serves "Hack the Planet" captive portal page.
 *                   Shows connected clients and activity log.
 *                   BACK       = stop and exit
 *
 * Character set for SSID editor (40 chars):
 *   space, A-Z (26), 0-9 (10), - _ . (3)
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "wifi_captive.h"

#include "bs/bs_wifi.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"

#include "esp_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "wifi_portal.h"

/* ── Config ─────────────────────────────────────────────────────────────── */

#define CP_MAX_SSID_LEN  32
#define CP_LOG_LINES      5
#define CP_LOG_LEN       48
#define CP_REFRESH_MS  1000
#define CP_ANIM_MS       40

/* ── Character set ───────────────────────────────────────────────────────── */

static const char k_charset[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
#define CP_CHARSET_LEN  ((int)(sizeof(k_charset) - 1))

static char charset_next(char c) {
    for (int i = 0; i < CP_CHARSET_LEN; i++)
        if (k_charset[i] == c) return k_charset[(i + 1) % CP_CHARSET_LEN];
    return k_charset[0];
}
static char charset_prev(char c) {
    for (int i = 0; i < CP_CHARSET_LEN; i++)
        if (k_charset[i] == c)
            return k_charset[(i + CP_CHARSET_LEN - 1) % CP_CHARSET_LEN];
    return k_charset[0];
}

/* ── Phase ───────────────────────────────────────────────────────────────── */

typedef enum { CP_SSID_EDIT, CP_CH_SELECT, CP_RUNNING } cp_phase_t;

/* ── State ───────────────────────────────────────────────────────────────── */

static cp_phase_t s_phase;
static char       s_ssid[CP_MAX_SSID_LEN + 1];
static int        s_edit_pos;
static uint8_t    s_channel;

static int        s_prev_clients;

static char       s_log[CP_LOG_LINES][CP_LOG_LEN];
static int        s_log_head;
static int        s_log_count;

/* ── Log ─────────────────────────────────────────────────────────────────── */

static void cp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], CP_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][CP_LOG_LEN - 1] = '\0';
    s_log_head = (s_log_head + 1) % CP_LOG_LINES;
    if (s_log_count < CP_LOG_LINES) s_log_count++;
}

/* ── SSID helpers ────────────────────────────────────────────────────────── */

static const char* trimmed_ssid(void) {
    static char buf[CP_MAX_SSID_LEN + 1];
    strncpy(buf, s_ssid, CP_MAX_SSID_LEN);
    buf[CP_MAX_SSID_LEN] = '\0';
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    return buf;
}

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_ssid_edit(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;
    int cw  = bs_gfx_text_w("A", ts);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Captive Portal / SSID");

    bs_ui_draw_text_box(8, y, sw - 16, "SSID:", g_bs_theme.dim, ts2, true);
    y += lh2 + 2;

    int chars_per_row = (sw - 16) / (cw + 2);
    if (chars_per_row < 1) chars_per_row = 1;

    int view_start = s_edit_pos - chars_per_row / 2;
    if (view_start < 0) view_start = 0;
    if (view_start > CP_MAX_SSID_LEN - chars_per_row)
        view_start = CP_MAX_SSID_LEN - chars_per_row;
    if (view_start < 0) view_start = 0;

    for (int i = 0; i < chars_per_row && (view_start + i) < CP_MAX_SSID_LEN; i++) {
        int pos = view_start + i;
        int x   = 8 + i * (cw + 2);
        bool is_cursor = (pos == s_edit_pos);
        char ch = s_ssid[pos];
        char buf[2] = { ch == ' ' ? '_' : ch, '\0' };

        if (is_cursor) {
            bs_gfx_fill_rect(x - 1, y - 1, cw + 2, bs_gfx_text_h(ts) + 2,
                             g_bs_theme.accent);
            bs_gfx_text(x, y, buf, g_bs_theme.bg, ts);
        } else {
            bs_color_t c = (ch != ' ') ? g_bs_theme.primary : g_bs_theme.dim;
            bs_gfx_text(x, y, buf, c, ts);
        }
    }
    y += lh + 6;

    if (y + lh2 < bs_gfx_height() - bs_gfx_text_h(ts2) - 6) {
        char hint[48];
        char cur = s_ssid[s_edit_pos];
        snprintf(hint, sizeof(hint), "pos %d: '%c'  (UP/DOWN cycle)",
                 s_edit_pos + 1, cur == ' ' ? ' ' : cur);
        bs_ui_draw_text_box(8, y, sw - 16, hint, g_bs_theme.dim, ts2, true);
    }

    bs_ui_draw_hint("UP/DN=char  L/R=move  SEL=next  BACK=exit");
    bs_gfx_present();
}

static void draw_ch_select(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Captive Portal / Channel");

    char buf[48];
    snprintf(buf, sizeof(buf), "SSID: %.32s", trimmed_ssid());
    bs_ui_draw_text_box(8, y, bs_gfx_width() - 16, buf, g_bs_theme.dim, ts2, true);
    y += lh2 + 8;

    snprintf(buf, sizeof(buf), "Channel: %d", s_channel);
    bs_ui_draw_text_box(8, y, bs_gfx_width() - 16, buf, g_bs_theme.accent, ts, true);
    y += lh + 4;

    if (y + lh2 < bs_gfx_height() - bs_gfx_text_h(ts2) - 6)
        bs_ui_draw_text_box(8, y, bs_gfx_width() - 16, "(1-13)", g_bs_theme.dim, ts2, true);

    bs_ui_draw_hint("UP/DN=channel  SELECT=start  BACK=edit");
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
    bs_ui_draw_header("Captive Portal");

    char buf[80];
    snprintf(buf, sizeof(buf), "AP: %.38s  ch%d", trimmed_ssid(), s_channel);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts, true);
    y += lh;

    wifi_sta_list_t sta = (wifi_sta_list_t){0};
    esp_wifi_ap_get_sta_list(&sta);
    snprintf(buf, sizeof(buf), "Clients: %d   IP: 192.168.4.1", sta.num);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts2, true);
    y += lh2 + 4;

    bs_gfx_fill_rect(4, y, sw - 8, 1, g_bs_theme.dim);
    y += 5;

    int hint_h   = bs_gfx_text_h(ts2) + 6;
    int max_y    = bs_gfx_height() - hint_h;
    int n_creds  = wifi_portal_cred_count();

    if (n_creds > 0) {
        snprintf(buf, sizeof(buf), "Creds: %d", n_creds);
        if (y + lh2 <= max_y) {
            bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts2, true);
            y += lh2;
        }
        const wifi_portal_cred_t* last = wifi_portal_get_cred(n_creds - 1);
        if (last && y + lh2 <= max_y) {
            snprintf(buf, sizeof(buf), "  %.20s / %.20s",
                     last->user, last->pass);
            bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts2, true);
            y += lh2;
        }
        if (y < max_y)
            bs_gfx_fill_rect(4, y, sw - 8, 1, g_bs_theme.dim);
        y += 4;
    }

    for (int i = 0; i < s_log_count && i < CP_LOG_LINES; i++) {
        int idx = (s_log_head - s_log_count + i + CP_LOG_LINES * 2) % CP_LOG_LINES;
        bs_color_t c = (i == s_log_count - 1) ? g_bs_theme.accent : g_bs_theme.dim;
        int line_y = y + i * lh2;
        if (line_y + lh2 > max_y) break;
        bs_ui_draw_text_box(8, line_y, sw - 16, s_log[idx], c, ts2, true);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

void wifi_captive_run(const bs_arch_t* arch) {
    s_phase        = CP_SSID_EDIT;
    s_channel      = 1;
    s_edit_pos     = 0;
    s_prev_clients = 0;
    s_log_head     = 0;
    s_log_count    = 0;

    /* Default SSID: "FreeWifi" padded with spaces */
    memset(s_ssid, ' ', CP_MAX_SSID_LEN);
    s_ssid[CP_MAX_SSID_LEN] = '\0';
    const char* def = "FreeWifi";
    for (int i = 0; def[i] && i < CP_MAX_SSID_LEN; i++)
        s_ssid[i] = def[i];

    bool dirty       = true;
    uint32_t last_refresh = 0;
    uint32_t last_anim_ms = 0;

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        /* ── Portal service (RUNNING) ───────────────────────────────────── */
        if (s_phase == CP_RUNNING && wifi_portal_active()) {
            wifi_portal_poll();

            wifi_sta_list_t sta = (wifi_sta_list_t){0};
            esp_wifi_ap_get_sta_list(&sta);
            if (sta.num > s_prev_clients) {
                for (int i = s_prev_clients; i < (int)sta.num; i++) {
                    cp_log("Client %02X:%02X:%02X:.. joined",
                           sta.sta[i].mac[0],
                           sta.sta[i].mac[1],
                           sta.sta[i].mac[2]);
                }
                dirty = true;
            } else if (sta.num < s_prev_clients) {
                cp_log("Client disconnected");
                dirty = true;
            }
            s_prev_clients = sta.num;
        }

        /* ── Input ──────────────────────────────────────────────────────── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (s_phase) {

            case CP_SSID_EDIT:
                if (nav == BS_NAV_BACK) { wifi_portal_stop(); return; }
                else if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    s_ssid[s_edit_pos] = charset_prev(s_ssid[s_edit_pos]);
                    dirty = true;
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    s_ssid[s_edit_pos] = charset_next(s_ssid[s_edit_pos]);
                    dirty = true;
                } else if (nav == BS_NAV_LEFT) {
                    if (s_edit_pos > 0) { s_edit_pos--; dirty = true; }
                } else if (nav == BS_NAV_RIGHT) {
                    if (s_edit_pos < CP_MAX_SSID_LEN - 1) { s_edit_pos++; dirty = true; }
                } else if (nav == BS_NAV_SELECT) {
                    s_phase = CP_CH_SELECT;
                    dirty   = true;
                }
                break;

            case CP_CH_SELECT:
                if (nav == BS_NAV_BACK) { s_phase = CP_SSID_EDIT; dirty = true; }
                else if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    if (s_channel < 13) { s_channel++; dirty = true; }
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    if (s_channel > 1)  { s_channel--; dirty = true; }
                } else if (nav == BS_NAV_SELECT) {
                    const char* ssid = trimmed_ssid();
                    cp_log("Starting AP: %.28s ch%d", ssid, s_channel);
                    if (wifi_portal_start(ssid, s_channel, NULL)) {
                        cp_log("AP up at 192.168.4.1");
                        cp_log("Serving captive portal...");
                        s_phase      = CP_RUNNING;
                        last_refresh = now;
                    } else {
                        cp_log("AP start FAILED");
                    }
                    dirty = true;
                }
                break;

            case CP_RUNNING:
                if (nav == BS_NAV_BACK) {
                    cp_log("Portal stopped.");
                    wifi_portal_stop();
                    return;
                }
                break;
            }
        }

        bool anim_due = bs_ui_carousel_enabled()
                     && (uint32_t)(now - last_anim_ms) >= (uint32_t)CP_ANIM_MS;

        /* ── Draw ───────────────────────────────────────────────────────── */
        if (dirty || anim_due) {
            dirty = false;
            switch (s_phase) {
            case CP_SSID_EDIT:  draw_ssid_edit(); break;
            case CP_CH_SELECT:  draw_ch_select(); break;
            case CP_RUNNING:    draw_running();   break;
            }
            if (anim_due) last_anim_ms = now;
        }

        if (s_phase == CP_RUNNING && (now - last_refresh) >= (uint32_t)CP_REFRESH_MS) {
            last_refresh = now;
            dirty = true;
        }

#if defined(VARIANT_TPAGER)
        arch->delay_ms(s_phase == CP_RUNNING ? 2 : 1);
#else
        arch->delay_ms(s_phase == CP_RUNNING ? 10 : 5);
#endif
    }
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
