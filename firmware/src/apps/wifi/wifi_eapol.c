/*
 * wifi_eapol.c - EAPOL handshake capture UI.
 *
 * Menu:
 *   Start
 *   Channel:  AUTO-HOP / FIXED
 *     Fixed ch: N        (shown only when FIXED)
 *   Deauth:   OFF / PAIRS / BCAST / BOTH
 *   Back
 *
 * Deauth submenu:
 *   Mode, interval, burst, reason, AP->STA vs bidirectional, disassoc on/off.
 *
 * Running screen:
 *   Status bar: channel, deauth state
 *   Stats:      EAPOL frames, completed pairs, learned AP/STA links, forged frames
 *   PCAP path shown at bottom when writing
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

typedef enum { EP_MENU, EP_DEAUTH, EP_RUNNING } ep_state_t;

typedef enum { EP_CH_AUTO = 0, EP_CH_FIXED, EP_CH_COUNT } ep_chmode_t;

static ep_state_t          s_state;
static ep_chmode_t         s_chmode;
static uint8_t             s_channel;
static eapol_deauth_cfg_t  s_deauth_cfg;
static char                s_pcap_path[64];

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

/* ── Deauth config helpers ──────────────────────────────────────────────── */

static const char* deauth_mode_name(eapol_deauth_mode_t m) {
    switch (m) {
        case EAPOL_DEAUTH_LEARNED_PAIRS: return "PAIRS";
        case EAPOL_DEAUTH_BROADCAST_APS: return "BCAST";
        case EAPOL_DEAUTH_PAIRS_AND_BCAST: return "BOTH";
        default: return "OFF";
    }
}

static void deauth_cfg_reset(void) {
    memset(&s_deauth_cfg, 0, sizeof s_deauth_cfg);
    s_deauth_cfg.mode        = EAPOL_DEAUTH_OFF;
    s_deauth_cfg.interval_ms = 5000;
    s_deauth_cfg.burst       = 3;
    s_deauth_cfg.reason      = 7;
    s_deauth_cfg.bidir       = true;
    s_deauth_cfg.disassoc    = true;
}

static int opt_index_u32(const uint32_t* opts, int n, uint32_t v) {
    int best = 0;
    uint32_t best_d = (opts[0] > v) ? (opts[0] - v) : (v - opts[0]);
    for (int i = 1; i < n; i++) {
        uint32_t d = (opts[i] > v) ? (opts[i] - v) : (v - opts[i]);
        if (d < best_d) { best = i; best_d = d; }
    }
    return best;
}

static int opt_index_u8(const uint8_t* opts, int n, uint8_t v) {
    int best = 0;
    int best_d = (opts[0] > v) ? (opts[0] - v) : (v - opts[0]);
    for (int i = 1; i < n; i++) {
        int d = (opts[i] > v) ? (opts[i] - v) : (v - opts[i]);
        if (d < best_d) { best = i; best_d = d; }
    }
    return best;
}

static int opt_index_u16(const uint16_t* opts, int n, uint16_t v) {
    int best = 0;
    int best_d = (opts[0] > v) ? (opts[0] - v) : (v - opts[0]);
    for (int i = 1; i < n; i++) {
        int d = (opts[i] > v) ? (opts[i] - v) : (v - opts[i]);
        if (d < best_d) { best = i; best_d = d; }
    }
    return best;
}

static void deauth_adjust(int item, int dir) {
    static const uint32_t ivls[] = { 1000, 2000, 5000, 10000, 30000 };
    static const uint8_t  bursts[] = { 1, 2, 3, 5, 8 };
    static const uint16_t reasons[] = { 1, 4, 7, 8 };

    if (item == 0) {
        int m = (int)s_deauth_cfg.mode + dir;
        if (m < 0) m = 3;
        if (m > 3) m = 0;
        s_deauth_cfg.mode = (eapol_deauth_mode_t)m;
    } else if (item == 1) {
        int i = opt_index_u32(ivls, (int)(sizeof ivls / sizeof ivls[0]), s_deauth_cfg.interval_ms);
        i += dir;
        int n = (int)(sizeof ivls / sizeof ivls[0]);
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        s_deauth_cfg.interval_ms = ivls[i];
    } else if (item == 2) {
        int i = opt_index_u8(bursts, (int)(sizeof bursts / sizeof bursts[0]), s_deauth_cfg.burst);
        i += dir;
        int n = (int)(sizeof bursts / sizeof bursts[0]);
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        s_deauth_cfg.burst = bursts[i];
    } else if (item == 3) {
        int i = opt_index_u16(reasons, (int)(sizeof reasons / sizeof reasons[0]), s_deauth_cfg.reason);
        i += dir;
        int n = (int)(sizeof reasons / sizeof reasons[0]);
        if (i < 0) i = n - 1;
        if (i >= n) i = 0;
        s_deauth_cfg.reason = reasons[i];
    } else if (item == 4) {
        s_deauth_cfg.bidir = !s_deauth_cfg.bidir;
    } else if (item == 5) {
        s_deauth_cfg.disassoc = !s_deauth_cfg.disassoc;
    }
}

/* ── Menu helpers ────────────────────────────────────────────────────────── */

#define EP_ITEM_START    0
#define EP_ITEM_CHMODE   1
#define EP_ITEM_FIXEDCH  2   /* only active when FIXED */
#define EP_ITEM_DEAUTH   3   /* index shifts by 1 when FIXED hidden */
#define EP_ITEM_BACK_F   4
#define EP_ITEM_BACK_A   3

#define EP_DAUTH_ITEMS   7
#define EP_DAUTH_BACK    6

static int menu_item_count(void) {
    return (s_chmode == EP_CH_FIXED) ? 5 : 4;
}

static int cursor_to_item(int cursor) {
    if (s_chmode == EP_CH_AUTO && cursor >= EP_ITEM_FIXEDCH)
        return cursor + 1;
    return cursor;
}

/* ── Draw: menu ──────────────────────────────────────────────────────────── */

static void draw_menu(int cursor) {
    float ts  = bs_ui_text_scale();
    int   cy  = bs_ui_content_y();
    int   lh  = bs_ui_row_h(ts);
    int   sw  = bs_gfx_width();

    const char* chmodes[] = { "AUTO-HOP", "FIXED" };
    char chmode_buf[28], ch_buf[24], deauth_buf[28];
    snprintf(chmode_buf, sizeof chmode_buf, "Channel: %-8s", chmodes[s_chmode]);
    snprintf(ch_buf,     sizeof ch_buf,     "  Fixed ch: %d", s_channel);
    snprintf(deauth_buf, sizeof deauth_buf, "Deauth: %-5s", deauth_mode_name(s_deauth_cfg.mode));

    int n = menu_item_count();
    const char* items[5];
    items[0] = "Start";
    items[1] = chmode_buf;
    items[2] = (s_chmode == EP_CH_FIXED) ? ch_buf : NULL;
    items[3] = deauth_buf;
    items[4] = (s_chmode == EP_CH_FIXED) ? "Back" : NULL;
    if (s_chmode == EP_CH_AUTO) {
        items[2] = deauth_buf;
        items[3] = "Back";
        items[4] = NULL;
    }

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
    bs_ui_draw_hint("SELECT=pick  <<=>> tune  BACK=exit");
    bs_gfx_present();
}

/* ── Draw: deauth submenu ───────────────────────────────────────────────── */

static void draw_deauth_menu(int cursor) {
    float ts  = bs_ui_text_scale();
    int   cy  = bs_ui_content_y();
    int   lh  = bs_ui_row_h(ts);
    int   sw  = bs_gfx_width();
    char  line0[28], line1[28], line2[28], line3[28], line4[28], line5[28];

    snprintf(line0, sizeof line0, "Mode: %-5s", deauth_mode_name(s_deauth_cfg.mode));
    snprintf(line1, sizeof line1, "Interval: %lus", (unsigned long)(s_deauth_cfg.interval_ms / 1000));
    snprintf(line2, sizeof line2, "Burst: %u", (unsigned)s_deauth_cfg.burst);
    snprintf(line3, sizeof line3, "Reason: %u", (unsigned)s_deauth_cfg.reason);
    snprintf(line4, sizeof line4, "Forge: %s", s_deauth_cfg.bidir ? "BOTH" : "AP->STA");
    snprintf(line5, sizeof line5, "Disassoc: %s", s_deauth_cfg.disassoc ? "ON" : "OFF");

    const char* items[EP_DAUTH_ITEMS] = {
        line0, line1, line2, line3, line4, line5, "Back"
    };

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("EAPOL Deauth");

    int visible = bs_ui_list_visible(ts);
    int scroll  = 0;
    bs_ui_list_clamp_scroll(cursor, &scroll, EP_DAUTH_ITEMS, visible);

    for (int i = scroll, drawn = 0; i < EP_DAUTH_ITEMS && drawn < visible; i++, drawn++) {
        bool sel = (i == cursor);
        int y = cy + drawn * lh;
        if (sel) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);
        bs_ui_draw_text_box(8, y, sw - 16, items[i],
                            sel ? g_bs_theme.accent : g_bs_theme.primary, ts, sel);
    }
    bs_ui_draw_scroll_arrows(scroll, EP_DAUTH_ITEMS, visible);
    bs_ui_draw_hint("<<>>:change  SELECT=change/back");
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
    char  buf[96];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("EAPOL [RUNNING]");

    if (s_chmode == EP_CH_FIXED)
        snprintf(buf, sizeof buf, "Ch:%u deauth:%s", s_channel, deauth_mode_name(s_deauth_cfg.mode));
    else
        snprintf(buf, sizeof buf, "Ch:auto deauth:%s", deauth_mode_name(s_deauth_cfg.mode));
    bs_ui_draw_text_box(8, cy, sw - 16, buf, g_bs_theme.secondary, ts2, true);

    uint32_t total = eapol_svc_eapol_count();
    int      pairs = eapol_svc_pair_count();
    int      links = eapol_svc_learned_count();
    int      aps   = eapol_svc_ap_count();
    uint32_t tx    = eapol_svc_deauth_frames();
    uint32_t pcap  = eapol_svc_pcap_frames();
    snprintf(buf, sizeof buf, "EAPOL:%lu Pair:%d AP:%d L:%d",
             (unsigned long)total, pairs, aps, links);
    bs_ui_draw_text_box(8, cy + lh2, sw - 16, buf, g_bs_theme.primary, ts2, true);
    snprintf(buf, sizeof buf, "pcap:%lu  tx:%lu",
             (unsigned long)pcap, (unsigned long)tx);
    bs_ui_draw_text_box(8, cy + 2 * lh2, sw - 16, buf, g_bs_theme.primary, ts2, true);

    int sep_y  = cy + 3 * lh2 + 3;
    bs_gfx_fill_rect(0, sep_y, sw, 1, g_bs_theme.dim);
    int list_y = sep_y + 3;

    if (total == 0) {
        if (links == 0)
            bs_gfx_text(8, list_y, "learning AP/client traffic...", g_bs_theme.dim, ts2);
        else
            bs_gfx_text(8, list_y, "waiting for EAPOL frames...", g_bs_theme.dim, ts2);
    } else if (pairs == 0) {
        snprintf(buf, sizeof buf, "%lu EAPOL frame%s — waiting pair",
                 (unsigned long)total, total == 1 ? "" : "s");
        bs_gfx_text(8, list_y, buf, g_bs_theme.dim, ts2);
    } else {
        snprintf(buf, sizeof buf, "%d complete pair%s captured!",
                 pairs, pairs == 1 ? "" : "s");
        bs_gfx_text(8, list_y, buf, g_bs_theme.accent, ts);
    }

    if (s_pcap_path[0]) {
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
    deauth_cfg_reset();
    s_pcap_path[0] = '\0';
    int cursor = 0;
    int dcursor = 0;

    uint32_t prev_ms = arch->millis();
    bool     dirty   = true;

    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        if (s_state == EP_RUNNING) eapol_svc_tick(now);

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
                            eapol_svc_start_cfg(
                                s_chmode == EP_CH_FIXED ? s_channel : 0,
                                NULL,
                                &s_deauth_cfg,
                                s_pcap_path[0] ? s_pcap_path : NULL);
                            s_state = EP_RUNNING;
                        } else if (item == EP_ITEM_CHMODE) {
                            s_chmode = (s_chmode == EP_CH_AUTO) ? EP_CH_FIXED : EP_CH_AUTO;
                        } else if (item == EP_ITEM_FIXEDCH) {
                            s_channel = (s_channel % 13) + 1;
                        } else if (item == EP_ITEM_DEAUTH) {
                            s_state = EP_DEAUTH;
                            dcursor = 0;
                        } else {
                            return;
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
                            deauth_adjust(0, dir);
                        }
                        dirty = true;
                        break;
                    }
                    case BS_NAV_BACK: return;
                    default: break;
                }
            } else if (s_state == EP_DEAUTH) {
                switch (nav) {
                    case BS_NAV_UP:   case BS_NAV_PREV:
                        dcursor = (dcursor + EP_DAUTH_ITEMS - 1) % EP_DAUTH_ITEMS; dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        dcursor = (dcursor + 1) % EP_DAUTH_ITEMS; dirty = true; break;
                    case BS_NAV_LEFT:
                        if (dcursor != EP_DAUTH_BACK) deauth_adjust(dcursor, -1);
                        dirty = true; break;
                    case BS_NAV_RIGHT:
                        if (dcursor != EP_DAUTH_BACK) deauth_adjust(dcursor, 1);
                        dirty = true; break;
                    case BS_NAV_SELECT:
                        if (dcursor == EP_DAUTH_BACK) s_state = EP_MENU;
                        else deauth_adjust(dcursor, 1);
                        dirty = true; break;
                    case BS_NAV_BACK:
                        s_state = EP_MENU; dirty = true; break;
                    default: break;
                }
            } else { /* EP_RUNNING */
                if (nav == BS_NAV_BACK) {
                    eapol_svc_stop();
                    s_state = EP_MENU;
                    cursor  = 0;
                    dirty   = true;
                }
            }
        }

        if (dirty || s_state == EP_RUNNING) {
            if (s_state == EP_MENU)         draw_menu(cursor);
            else if (s_state == EP_DEAUTH) draw_deauth_menu(dcursor);
            else                            draw_running();
            dirty = false;
        }
        arch->delay_ms(50);
    }
}

#endif /* BS_HAS_WIFI */
