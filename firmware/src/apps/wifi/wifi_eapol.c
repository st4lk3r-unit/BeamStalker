/*
 * wifi_eapol.c - EAPOL handshake capture UI.
 *
 * Menu:
 *   Start
 *   Channel:  AUTO-HOP / FIXED
 *     Fixed ch: N        (shown only when FIXED)
 *   Deauth:   OFF / ON   (broadcast deauth every 5 s to force reauth)
 *   Back
 *
 * Running screen:
 *   Status bar: channel, deauth state
 *   Stats:      EAPOL frames total, complete pairs (M1+M2)
 *   Pair list:  BSSID  STA  [PARTIAL/COMPLETE]
 *   PCAP path shown at bottom when writing
 *
 * PCAP files are auto-numbered: BeamStalker/wifi/eapol/eapol_NNNN.pcap
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_eapol.h"
#include "wifi_eapol_svc.h"
#include "beamstalker.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"
#include "bs/bs_fs.h"

#include <stdio.h>
#include <string.h>

/* ── State ─────────────────────────────────────────────────────────────── */

typedef enum { EP_MENU, EP_RUNNING } ep_state_t;

typedef enum { EP_CH_AUTO = 0, EP_CH_FIXED, EP_CH_COUNT } ep_chmode_t;

static ep_state_t  s_state;
static ep_chmode_t s_chmode;
static uint8_t     s_channel;
static bool        s_deauth;
static char        s_pcap_path[64];

/* ── PCAP auto-number ────────────────────────────────────────────────────── */

static void pcap_open_next(void) {
    if (!bs_fs_available()) { s_pcap_path[0] = '\0'; return; }
    bs_fs_mkdir_p(BS_PATH_EAPOL);
    for (int i = 0; i < 9999; i++) {
        snprintf(s_pcap_path, sizeof s_pcap_path,
                 BS_PATH_EAPOL "/eapol_%04d.pcap", i);
        if (!bs_fs_exists(s_pcap_path)) return;
    }
    s_pcap_path[0] = '\0';
}

/* ── Menu helpers ────────────────────────────────────────────────────────── */

/* Item indices — FIXED_CH row only present when s_chmode == EP_CH_FIXED */
#define EP_ITEM_START    0
#define EP_ITEM_CHMODE   1
#define EP_ITEM_FIXEDCH  2   /* only active when FIXED */
#define EP_ITEM_DEAUTH   3   /* index shifts by 1 when FIXED hidden */
#define EP_ITEM_BACK_F   4   /* back when FIXED shown */
#define EP_ITEM_BACK_A   3   /* back when AUTO (no FIXED_CH row) */

static int menu_item_count(void) {
    return (s_chmode == EP_CH_FIXED) ? 5 : 4;
}

/* Map cursor position to logical item, skipping hidden FIXED_CH row */
static int cursor_to_item(int cursor) {
    if (s_chmode == EP_CH_AUTO && cursor >= EP_ITEM_FIXEDCH)
        return cursor + 1;   /* skip item 2 */
    return cursor;
}

/* ── Draw: menu ──────────────────────────────────────────────────────────── */

static void draw_menu(int cursor) {
    float ts  = bs_ui_text_scale();
    int   cy  = bs_ui_content_y();
    int   lh  = bs_ui_row_h(ts);
    int   sw  = bs_gfx_width();

    const char* chmodes[] = { "AUTO-HOP", "FIXED" };
    char chmode_buf[28], ch_buf[24], deauth_buf[20];
    snprintf(chmode_buf, sizeof chmode_buf, "Channel: %-8s", chmodes[s_chmode]);
    snprintf(ch_buf,     sizeof ch_buf,     "  Fixed ch: %d", s_channel);
    snprintf(deauth_buf, sizeof deauth_buf, "Deauth:  %s", s_deauth ? "ON " : "OFF");

    int n = menu_item_count();
    const char* items[5];
    items[0] = "Start";
    items[1] = chmode_buf;
    items[2] = (s_chmode == EP_CH_FIXED) ? ch_buf : NULL;
    items[3] = (s_chmode == EP_CH_FIXED) ? deauth_buf : deauth_buf;
    items[4] = (s_chmode == EP_CH_FIXED) ? "Back" : NULL;
    if (s_chmode == EP_CH_AUTO) {
        items[2] = deauth_buf;
        items[3] = "Back";
        items[4] = NULL;
    }

    /* Build dense visible count */
    int cnt = 0;
    for (int i = 0; i < 5; i++) if (items[i]) cnt++;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("EAPOL Capture");

    int visible = bs_ui_list_visible(ts);
    int scroll  = 0;
    bs_ui_list_clamp_scroll(cursor, &scroll, n, visible);

    int row = 0, drawn = 0;
    for (int i = 0; i < 5; i++) {
        if (!items[i]) continue;
        if (row < scroll) { row++; continue; }
        if (drawn >= visible) break;
        bool sel = (row == cursor);
        int  y   = cy + drawn * lh;
        if (sel) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);
        bs_ui_draw_text_box(8, y, sw - 16, items[i],
                            sel ? g_bs_theme.accent : g_bs_theme.primary, ts, sel);
        row++; drawn++;
    }
    bs_ui_draw_scroll_arrows(scroll, n, visible);
    bs_ui_draw_hint("<<>>:toggle  SELECT=pick  BACK=exit");
    bs_gfx_present();
}

/* ── Draw: running ───────────────────────────────────────────────────────── */

static void draw_running(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    int   sw  = bs_gfx_width();
    int   cy  = bs_ui_content_y();
    int   lh  = bs_gfx_text_h(ts)  + 4;
    int   lh2 = bs_gfx_text_h(ts2) + 3;
    char  buf[80];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("EAPOL [RUNNING]");

    /* Status line */
    if (s_chmode == EP_CH_FIXED)
        snprintf(buf, sizeof buf, "Ch:%u  deauth:%s", s_channel, s_deauth?"ON":"OFF");
    else
        snprintf(buf, sizeof buf, "Ch:auto-hop  deauth:%s", s_deauth?"ON":"OFF");
    bs_ui_draw_text_box(8, cy, sw - 16, buf, g_bs_theme.secondary, ts2, true);

    /* Stats */
    uint32_t total = eapol_svc_eapol_count();
    int      pairs = eapol_svc_pair_count();
    uint32_t pcap  = eapol_svc_pcap_frames();
    snprintf(buf, sizeof buf, "EAPOL:%lu  Pairs:%d  pcap:%lu",
             (unsigned long)total, pairs, (unsigned long)pcap);
    bs_ui_draw_text_box(8, cy + lh2, sw - 16, buf, g_bs_theme.primary, ts2, true);

    /* Separator */
    int sep_y  = cy + 2 * lh2 + 3;
    bs_gfx_fill_rect(0, sep_y, sw, 1, g_bs_theme.dim);
    int list_y = sep_y + 3;

    /* Empty state */
    if (total == 0) {
        bs_gfx_text(8, list_y, "waiting for EAPOL frames...", g_bs_theme.dim, ts2);
    } else if (pairs == 0) {
        snprintf(buf, sizeof buf, "%lu EAPOL frame%s — waiting for pair...",
                 (unsigned long)total, total == 1 ? "" : "s");
        bs_gfx_text(8, list_y, buf, g_bs_theme.dim, ts2);
    } else {
        snprintf(buf, sizeof buf, "%d handshake pair%s captured!",
                 pairs, pairs == 1 ? "" : "s");
        bs_gfx_text(8, list_y, buf, g_bs_theme.accent, ts);
    }

    /* pcap hint */
    if (s_pcap_path[0]) {
        /* Show just the filename to fit the screen */
        const char* fname = s_pcap_path;
        const char* slash = strrchr(s_pcap_path, '/');
        if (slash) fname = slash + 1;
        snprintf(buf, sizeof buf, "pcap: %s", fname);
        int hint_y = cy + bs_ui_content_h() - lh2 - 2;
        bs_gfx_text(8, hint_y, buf, g_bs_theme.dim, ts2);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wifi_eapol_run(const bs_arch_t* arch) {
    s_state   = EP_MENU;
    s_chmode  = EP_CH_AUTO;
    s_channel = 1;
    s_deauth  = false;
    s_pcap_path[0] = '\0';
    int cursor = 0;

    uint32_t prev_ms = arch->millis();
    bool     dirty   = true;

    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        /* Tick running service */
        if (s_state == EP_RUNNING) {
            eapol_svc_tick(now);
        }

        /* Input */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            if (s_state == EP_MENU) {
                int n = menu_item_count();
                switch (nav) {
                    case BS_NAV_UP:   case BS_NAV_PREV:
                        cursor = (cursor + n - 1) % n; dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        cursor = (cursor + 1) % n; dirty = true; break;
                    case BS_NAV_SELECT: {
                        int item = cursor_to_item(cursor);
                        if (item == EP_ITEM_START) {
                            pcap_open_next();
                            eapol_svc_init(arch);
                            eapol_svc_start(
                                s_chmode == EP_CH_FIXED ? s_channel : 0,
                                NULL,          /* no BSSID filter in UI mode */
                                s_deauth,
                                0,             /* default deauth interval */
                                s_pcap_path[0] ? s_pcap_path : NULL);
                            s_state = EP_RUNNING;
                        } else if (item == EP_ITEM_CHMODE) {
                            s_chmode = (s_chmode == EP_CH_AUTO) ? EP_CH_FIXED : EP_CH_AUTO;
                        } else if (item == EP_ITEM_FIXEDCH) {
                            s_channel = (s_channel % 13) + 1;
                        } else if (item == EP_ITEM_DEAUTH) {
                            s_deauth = !s_deauth;
                        } else {
                            return; /* Back */
                        }
                        dirty = true;
                        break;
                    }
                    case BS_NAV_LEFT: case BS_NAV_RIGHT: {
                        int item = cursor_to_item(cursor);
                        int dir  = (nav == BS_NAV_LEFT) ? -1 : 1;
                        if (item == EP_ITEM_CHMODE) {
                            s_chmode = (s_chmode == EP_CH_AUTO) ? EP_CH_FIXED : EP_CH_AUTO;
                        } else if (item == EP_ITEM_FIXEDCH) {
                            if (dir > 0) s_channel = (s_channel % 13) + 1;
                            else         s_channel = (s_channel == 1) ? 13 : s_channel - 1;
                        } else if (item == EP_ITEM_DEAUTH) {
                            s_deauth = !s_deauth;
                        }
                        dirty = true;
                        break;
                    }
                    case BS_NAV_BACK: return;
                    default: break;
                }
            } else { /* EP_RUNNING */
                if (nav == BS_NAV_BACK) {
                    eapol_svc_stop();
                    bs_wifi_deinit();
                    s_state = EP_MENU;
                    cursor  = 0;
                    dirty   = true;
                }
            }
        }

        if (dirty || s_state == EP_RUNNING) {
            if (s_state == EP_MENU)    draw_menu(cursor);
            else                       draw_running();
            dirty = false;
        }
        arch->delay_ms(50);
    }
}

#endif /* BS_HAS_WIFI */
