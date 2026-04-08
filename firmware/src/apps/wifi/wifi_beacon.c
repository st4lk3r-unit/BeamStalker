/*
 * wifi_beacon.c - Beacon spam UI (thin wrapper over wifi_beacon_svc).
 *
 * Handles the menu (mode/charset/prefix/repeat) and running screen.
 * All TX logic lives in wifi_beacon_svc.c.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_beacon.h"
#include "wifi_beacon_svc.h"
#include "wifi_common.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_keys.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
#include "bs/bs_fs.h"
#include "bs/bs_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Config ─────────────────────────────────────────────────────────────── */

#define MAX_FILE_SSIDS   128
#define SSID_MAX_LEN      33
#define PREFIX_MAX_LEN    21

static const int         k_repeat_vals[]  = {1, 2, 3, 5, 10};
static const char* const k_repeat_names[] = {"1×","2×","3×","5×","10×"};
#define N_REPEAT 5

#define RUNNING_DRAW_INTERVAL_MS 2000

/* ── UI state ────────────────────────────────────────────────────────────── */

typedef enum { BCN_MENU, BCN_EDIT_PREFIX, BCN_RUNNING } bcn_ui_state_t;

typedef enum {
    ROW_MODE=0, ROW_CHARSET=1, ROW_PREFIX=2, ROW_REPEAT=3,
    ROW_START=4, ROW_BACK=5,
} bcn_row_t;

static bcn_ui_state_t s_ui_state;
static beacon_svc_mode_t s_mode;
static wifi_charset_t    s_charset;
static int               s_cursor;
static bool              s_dirty;
static char              s_prefix[PREFIX_MAX_LEN];
static int               s_repeat_idx;
static uint32_t          s_last_draw_ms;

static char s_file_ssids[MAX_FILE_SSIDS][SSID_MAX_LEN];
static int  s_file_ssid_count;

/* ── File loader ─────────────────────────────────────────────────────────── */

static void load_ssids_from_file(void) {
    s_file_ssid_count = 0;
    if (!bs_fs_available()) return;
    bs_file_t f = bs_fs_open("/wifi/ssids.txt", "r");
    if (!f) return;
    int idx = 0;
    while (idx < MAX_FILE_SSIDS) {
        char line[SSID_MAX_LEN + 4];
        int  pos = 0; bool got = false; char c;
        while (pos < (int)sizeof(line) - 1) {
            if (bs_fs_read(f, &c, 1) != 1) break;
            if (c == '\n') { got = true; break; }
            if (c != '\r') line[pos++] = c;
            got = true;
        }
        if (!got && pos == 0) break;
        line[pos] = '\0';
        if (pos > 0 && pos <= 32) {
            strncpy(s_file_ssids[idx], line, SSID_MAX_LEN - 1);
            s_file_ssids[idx][SSID_MAX_LEN - 1] = '\0';
            idx++;
        }
    }
    bs_fs_close(f);
    s_file_ssid_count = idx;
}

/* ── Row list ────────────────────────────────────────────────────────────── */

static int get_rows(bcn_row_t rows[6]) {
    int n = 0;
    rows[n++] = ROW_MODE;
    if (s_mode == BEACON_MODE_RANDOM)  rows[n++] = ROW_CHARSET;
    if (s_mode == BEACON_MODE_CUSTOM)  rows[n++] = ROW_PREFIX;
    rows[n++] = ROW_REPEAT;
    rows[n++] = ROW_START;
    rows[n++] = ROW_BACK;
    return n;
}

/* ── Text input screen ───────────────────────────────────────────────────── */

static void run_prefix_edit(const bs_arch_t* arch) {
    int len = (int)strlen(s_prefix);
    bool dirty = true;
    for (;;) {
        bs_key_t key;
        while (bs_keys_poll(&key)) {
            if (key.id == BS_KEY_CHAR) {
                if (key.ch >= 0x20 && key.ch <= 0x7E && key.ch != ' '
                    && len < PREFIX_MAX_LEN - 1) {
                    s_prefix[len++] = key.ch;
                    s_prefix[len]   = '\0';
                    dirty = true;
                }
            } else if (key.id == BS_KEY_BACK || key.id == BS_KEY_ESC) {
                if (len > 0) { s_prefix[--len] = '\0'; dirty = true; }
                else return;
            } else if (key.id == BS_KEY_ENTER) return;
        }
        if (dirty) {
            dirty = false;
            float ts = bs_ui_text_scale();
            int cy = bs_ui_content_y(), lh = bs_gfx_text_h(ts) + 4;
            char buf[PREFIX_MAX_LEN + 8];
            bs_gfx_clear(g_bs_theme.bg);
            bs_ui_draw_header("Custom Prefix");
            bs_gfx_text(8, cy, "Enter AP name prefix:", g_bs_theme.secondary, ts);
            snprintf(buf, sizeof buf, "%s_", s_prefix[0] ? s_prefix : "");
            bs_gfx_text(8, cy + lh + 2, buf, g_bs_theme.accent, ts);
            char preview[SSID_MAX_LEN + 16];
            snprintf(preview, sizeof preview, "e.g.  %s042",
                     s_prefix[0] ? s_prefix : "testAP");
            bs_gfx_text(8, cy + 2*lh + 6, preview, g_bs_theme.dim, ts);
            bs_ui_draw_hint("type  BACK=del  SELECT=ok");
            bs_gfx_present();
        }
        arch->delay_ms(5);
    }
}

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_menu(void) {
    float ts  = bs_ui_text_scale();
    int sw    = bs_gfx_width();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 6;
    int val_x = bs_gfx_text_w("Charset  ", ts) + 8;

    static const char* mode_names[] = { "RANDOM", "FILE", "CUSTOM" };

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Beacon Spam");

    bcn_row_t rows[6];
    int n = get_rows(rows);
    int hint_h  = bs_gfx_text_h(ts) + 6;
    int max_vis = (bs_gfx_height() - cy - hint_h) / lh;
    if (max_vis < 1) max_vis = 1;
    int scroll = s_cursor - max_vis / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > n - max_vis) scroll = n - max_vis;
    if (scroll < 0) scroll = 0;

    for (int ci = scroll; ci < n && (ci - scroll) < max_vis; ci++) {
        bcn_row_t row = rows[ci];
        bool sel = (ci == s_cursor);
        int  y   = cy + (ci - scroll) * lh;
        if (sel) bs_gfx_fill_rect(0, y - 2, sw, lh - 1, g_bs_theme.dim);
        bs_color_t lc = sel ? g_bs_theme.accent    : g_bs_theme.primary;
        bs_color_t vc = sel ? g_bs_theme.primary   : g_bs_theme.secondary;
        char left[24], right[28]; right[0] = '\0';
        switch (row) {
            case ROW_MODE:    snprintf(left, sizeof left, "Mode");
                              snprintf(right, sizeof right, "%s", mode_names[s_mode]); break;
            case ROW_CHARSET: snprintf(left, sizeof left, "Charset");
                              snprintf(right, sizeof right, "%s", k_wifi_charset_names[s_charset]); break;
            case ROW_PREFIX:  snprintf(left, sizeof left, "Prefix");
                              snprintf(right, sizeof right, "%s", s_prefix[0] ? s_prefix : "(empty)"); break;
            case ROW_REPEAT:  snprintf(left, sizeof left, "Repeat");
                              snprintf(right, sizeof right, "%s  (per SSID)", k_repeat_names[s_repeat_idx]); break;
            case ROW_START:   snprintf(left, sizeof left, "Start"); break;
            case ROW_BACK:    snprintf(left, sizeof left, "Back"); break;
        }
        bs_gfx_text(8, y, left, lc, ts);
        if (right[0]) bs_gfx_text(val_x, y, right, vc, ts);
    }

    if (s_mode == BEACON_MODE_FILE) {
        char hint[48];
        if (s_file_ssid_count > 0)
            snprintf(hint, sizeof hint, "%d SSIDs from /wifi/ssids.txt", s_file_ssid_count);
        else
            snprintf(hint, sizeof hint, "/wifi/ssids.txt not found");
        bs_ui_draw_hint(hint);
    } else {
        bs_ui_draw_hint("<<>>:cycle  SELECT=edit/start  BACK=exit");
    }
    bs_gfx_present();
}

static void draw_running(void) {
    float ts  = bs_ui_text_scale();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 4;
    char buf[64];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Beacon Spam [ON]");

    snprintf(buf, sizeof buf, "SSID     %s", beacon_svc_cur_ssid());
    bs_gfx_text(8, cy, buf, g_bs_theme.primary, ts);

    char bstr[18]; bs_wifi_bssid_str(beacon_svc_cur_bssid(), bstr);
    snprintf(buf, sizeof buf, "BSSID    %s  ch %d", bstr, (int)beacon_svc_cur_channel());
    bs_gfx_text(8, cy + lh, buf, g_bs_theme.secondary, ts);

    snprintf(buf, sizeof buf, "Frames   %lu", (unsigned long)beacon_svc_frames());
    bs_gfx_text(8, cy + 2*lh, buf, g_bs_theme.accent, ts);

    snprintf(buf, sizeof buf, "PPS      %lu", (unsigned long)beacon_svc_pps());
    bs_gfx_text(8, cy + 3*lh, buf, g_bs_theme.accent, ts);

    snprintf(buf, sizeof buf, "Repeat   %s  (%d left)",
             k_repeat_names[s_repeat_idx], beacon_svc_burst_left());
    bs_gfx_text(8, cy + 4*lh, buf, g_bs_theme.dim, ts);

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wifi_beacon_run(const bs_arch_t* arch) {
    beacon_svc_init(arch);
    s_ui_state   = BCN_MENU;
    s_mode       = BEACON_MODE_RANDOM;
    s_charset    = WIFI_CHARSET_ASCII;
    s_cursor     = 0;
    s_dirty      = true;
    s_repeat_idx = 2;
    s_last_draw_ms = 0;
    if (!s_prefix[0]) strncpy(s_prefix, "testAP", PREFIX_MAX_LEN - 1);
    load_ssids_from_file();

    if (!(bs_wifi_caps() & BS_WIFI_CAP_INJECT)) {
        float ts = bs_ui_text_scale();
        bs_gfx_clear(g_bs_theme.bg);
        bs_ui_draw_header("Beacon Spam");
        bs_gfx_text(8, bs_ui_content_y(), "No inject capability", g_bs_theme.warn, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_BACK) arch->delay_ms(10);
        return;
    }

    for (;;) {
        uint32_t now = arch->millis();

        /* ── Input ── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            if (s_ui_state == BCN_RUNNING) {
                if (nav == BS_NAV_BACK) { beacon_svc_stop(); s_ui_state = BCN_MENU; s_dirty = true; }
                continue;
            }
            bcn_row_t rows[6];
            int n = get_rows(rows);
            bcn_row_t row = rows[s_cursor];

#define CYCLE_LEFT(idx, cnt)  idx = ((idx) + (cnt) - 1) % (cnt)
#define CYCLE_RIGHT(idx, cnt) idx = ((idx) + 1) % (cnt)

            switch (nav) {
                case BS_NAV_UP: case BS_NAV_PREV:
                    s_cursor = (s_cursor + n - 1) % n; s_dirty = true; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    s_cursor = (s_cursor + 1) % n; s_dirty = true; break;
                case BS_NAV_SELECT: case BS_NAV_RIGHT:
                    if (row == ROW_MODE) {
                        s_mode = (beacon_svc_mode_t)((s_mode + 1) % 3);
                        if (s_cursor >= get_rows(rows)) s_cursor = 0;
                        s_dirty = true;
                    } else if (row == ROW_CHARSET) {
                        CYCLE_RIGHT(s_charset, WIFI_CHARSET_COUNT); s_dirty = true;
                    } else if (row == ROW_PREFIX) {
                        run_prefix_edit(arch); s_dirty = true;
                    } else if (row == ROW_REPEAT) {
                        CYCLE_RIGHT(s_repeat_idx, N_REPEAT); s_dirty = true;
                    } else if (row == ROW_START) {
                        beacon_svc_start(s_mode, s_charset, s_prefix,
                                         k_repeat_vals[s_repeat_idx],
                                         (const char(*)[33])s_file_ssids,
                                         s_file_ssid_count);
                        s_ui_state = BCN_RUNNING;
                        s_last_draw_ms = 0;
                        s_dirty = false;
                    } else if (row == ROW_BACK) {
                        return;
                    }
                    break;
                case BS_NAV_LEFT:
                    if (row == ROW_MODE) {
                        s_mode = (beacon_svc_mode_t)((s_mode + 2) % 3);
                        if (s_cursor >= get_rows(rows)) s_cursor = 0;
                        s_dirty = true;
                    } else if (row == ROW_CHARSET) {
                        CYCLE_LEFT(s_charset, WIFI_CHARSET_COUNT); s_dirty = true;
                    } else if (row == ROW_REPEAT) {
                        CYCLE_LEFT(s_repeat_idx, N_REPEAT); s_dirty = true;
                    }
                    break;
                case BS_NAV_BACK: return;
                default: break;
            }
#undef CYCLE_LEFT
#undef CYCLE_RIGHT
        }

        /* ── Tick & draw ── */
        if (s_ui_state == BCN_RUNNING) {
            beacon_svc_tick(now);
            if ((now - s_last_draw_ms) >= RUNNING_DRAW_INTERVAL_MS) {
                s_last_draw_ms = now;
                draw_running();
            }
        } else if (s_dirty) {
            s_dirty = false;
            draw_menu();
        }

        arch->delay_ms(1);
    }
}

#endif /* BS_HAS_WIFI */
