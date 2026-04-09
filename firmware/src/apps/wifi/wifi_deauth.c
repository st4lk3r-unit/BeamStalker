/*
 * wifi_deauth.c - Deauther UI (thin wrapper over wifi_deauth_svc).
 *
 * Flow:
 *   SCAN → AP_SELECT → SNIFF → CLIENT_SEL → ATTACK → DONE
 *
 * All attack logic lives in wifi_deauth_svc.c.  This file is pure
 * presentation: draw functions and input routing that call the service API.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_deauth.h"
#include "wifi_deauth_svc.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"

#include <stdio.h>
#include <string.h>

/* ── Layout helpers ──────────────────────────────────────────────────────── */

static int visible_rows(float ts) {
    return bs_ui_list_visible(ts);
}

/* ── Draw functions ──────────────────────────────────────────────────────── */

static void draw_scan(void) {
    float ts = bs_ui_text_scale();
    static uint8_t spin;
    const char* spinners = "-\\|/";
    char buf[32];
    snprintf(buf, sizeof buf, "Scanning...  %c", spinners[spin++ & 3]);
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Deauther");
    bs_gfx_text(8, bs_ui_content_y(), buf, g_bs_theme.primary, ts);
    bs_ui_draw_hint("BACK=cancel");
    bs_gfx_present();
}

static void draw_ap_list(int cursor, int scroll) {
    float ts  = bs_ui_text_scale();
    int sw    = bs_gfx_width();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 3;
    int vis   = visible_rows(ts);
    int n     = deauth_svc_ap_count();

    int sel_count = 0;
    for (int i = 0; i < n; i++)
        if (deauth_svc_ap(i)->selected) sel_count++;

    char title[32];
    snprintf(title, sizeof title, "Select APs  [%d/%d]", sel_count, n);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header(title);

    if (n == 0) {
        bs_gfx_text(8, cy, "No APs found", g_bs_theme.dim, ts);
    } else {
        for (int i = 0; i < vis; i++) {
            int idx = scroll + i;
            if (idx >= n) break;
            const wifi_ap_entry_t* e = deauth_svc_ap(idx);
            bool hl = (idx == cursor);
            int  y  = cy + i * lh;
            if (hl) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);
            char buf[48];
            snprintf(buf, sizeof buf, "[%c] %-16.16s ch%-2d %4d",
                     e->selected ? 'X' : ' ',
                     e->ap.ssid[0] ? e->ap.ssid : "<hidden>",
                     e->ap.channel, e->ap.rssi);
            bs_ui_draw_text_box(4, y, sw - 8, buf,
                                hl ? g_bs_theme.accent : g_bs_theme.primary, ts, hl);
        }
    }
    bs_ui_draw_hint("SELECT=toggle  RIGHT=next  BACK=exit");
    bs_gfx_present();
}

static void draw_sniff(void) {
    float ts  = bs_ui_text_scale();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 4;
    char buf[48];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Sniffing Clients");

    int sw_sniff = bs_gfx_width();
    snprintf(buf, sizeof buf, "Channel: %d  (%d / %d)",
             (int)deauth_svc_sniff_channel(),
             deauth_svc_sniff_ch_idx() + 1,
             deauth_svc_sniff_ch_count());
    bs_ui_draw_text_box(8, cy, sw_sniff - 16, buf, g_bs_theme.primary, ts, true);

    snprintf(buf, sizeof buf, "Clients: %d", deauth_svc_client_count());
    bs_ui_draw_text_box(8, cy + lh, sw_sniff - 16, buf, g_bs_theme.accent, ts, true);

    uint32_t elapsed = deauth_svc_sniff_elapsed_ms();
    uint32_t remain  = elapsed < 10000 ? (10000 - elapsed) / 1000 : 0;
    snprintf(buf, sizeof buf, "Time left: %lu s", (unsigned long)remain);
    bs_ui_draw_text_box(8, cy + 2 * lh, sw_sniff - 16, buf, g_bs_theme.secondary, ts, true);

    bs_ui_draw_hint("BACK=skip");
    bs_gfx_present();
}

static void draw_client_list(int cursor, int scroll) {
    float ts  = bs_ui_text_scale();
    int sw    = bs_gfx_width();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 3;
    int vis   = visible_rows(ts);
    int nc    = deauth_svc_client_count();
    int total = nc + 1;  /* +1 for Broadcast row */

    int sel_count = deauth_svc_broadcast_selected() ? 1 : 0;
    for (int i = 0; i < nc; i++)
        if (deauth_svc_client(i)->selected) sel_count++;

    char title[32];
    snprintf(title, sizeof title, "Select Clients [%d/%d]", sel_count, total);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header(title);

    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= total) break;
        bool hl = (idx == cursor);
        int  y  = cy + i * lh;
        if (hl) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);

        char buf[48];
        bool selected;
        if (idx == 0) {
            selected = deauth_svc_broadcast_selected();
            snprintf(buf, sizeof buf, "[%c] BROADCAST (FF:FF:FF:FF:FF:FF)",
                     selected ? 'X' : ' ');
        } else {
            const wifi_client_entry_t* cl = deauth_svc_client(idx - 1);
            selected = cl->selected;
            char mac[18];
            bs_wifi_bssid_str(cl->mac, mac);
            snprintf(buf, sizeof buf, "[%c] %s", selected ? 'X' : ' ', mac);
        }
            bs_ui_draw_text_box(4, y, sw - 8, buf,
                                hl ? g_bs_theme.accent : g_bs_theme.primary, ts, hl);
    }
    bs_ui_draw_hint("SELECT=toggle  RIGHT=attack  BACK=back");
    bs_gfx_present();
}

static void draw_attack(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts)  + 4;
    int lh2   = bs_gfx_text_h(ts2) + 3;
    char buf[DEAUTH_SVC_LOG_LEN];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Deauth [RUNNING]");

    int sw_atk = bs_gfx_width();
    snprintf(buf, sizeof buf, "Frames: %lu", (unsigned long)deauth_svc_frames());
    bs_ui_draw_text_box(8, cy,      sw_atk - 16, buf, g_bs_theme.accent, ts, true);
    snprintf(buf, sizeof buf, "PPS:    %lu", (unsigned long)deauth_svc_pps());
    bs_ui_draw_text_box(8, cy + lh, sw_atk - 16, buf, g_bs_theme.accent, ts, true);

    int sep_y = cy + 2 * lh + 4;
    bs_gfx_fill_rect(0, sep_y, bs_gfx_width(), 1, g_bs_theme.dim);
    int log_y = sep_y + 4;

    int lc = deauth_svc_log_count();
    for (int i = 0; i < lc; i++) {
        bs_color_t col = (i == lc - 1) ? g_bs_theme.primary : g_bs_theme.dim;
        bs_ui_draw_text_box(8, log_y + i * lh2, bs_gfx_width() - 16,
                            deauth_svc_log_line(i), col, ts2, (i == lc - 1));
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

static void draw_done(void) {
    float ts  = bs_ui_text_scale();
    int cy    = bs_ui_content_y();
    int lh    = bs_gfx_text_h(ts) + 4;
    char buf[32];
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Deauth Done");
    int sw_done = bs_gfx_width();
    snprintf(buf, sizeof buf, "Total frames: %lu", (unsigned long)deauth_svc_frames());
    bs_ui_draw_text_box(8, cy,      sw_done - 16, buf, g_bs_theme.accent, ts, true);
    bs_gfx_text(8, cy + lh, "SELECT=restart  BACK=exit", g_bs_theme.dim, ts);
    bs_gfx_present();
}

/* ── Scroll helpers ──────────────────────────────────────────────────────── */

static void scroll_clamp(int cursor, int* scroll, int vis) {
    if (cursor < *scroll)        *scroll = cursor;
    if (cursor >= *scroll + vis) *scroll = cursor - vis + 1;
    if (*scroll < 0)             *scroll = 0;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wifi_deauth_run(const bs_arch_t* arch) {
    deauth_svc_init(arch);
    deauth_svc_scan_aps();

    int cursor = 0, ap_scroll = 0, cl_scroll = 0;
    bool dirty = true;
    deauth_svc_state_t prev_state = DEAUTH_SVC_IDLE;

    uint32_t last_draw_ms = 0;
    static const uint32_t k_scan_draw_ms   = 100;   /* spinner update rate */
    static const uint32_t k_sniff_draw_ms  = 500;   /* sniff counter update */
    static const uint32_t k_carousel_ms    = 16;    /* carousel frame rate  */

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;
        deauth_svc_state_t state = deauth_svc_state();

        /* Detect automatic state transitions → reset cursor, force redraw */
        if (state != prev_state) {
            if (state == DEAUTH_SVC_AP_READY) {
                cursor = 0; ap_scroll = 0;
            } else if (state == DEAUTH_SVC_CLIENT_READY) {
                cursor = 0; cl_scroll = 0;
            }
            dirty = true;
            prev_state = state;
        }

        /* ── Input ── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            int vis;
            switch (state) {
                case DEAUTH_SVC_SCANNING:
                    if (nav == BS_NAV_BACK) return;
                    break;

                case DEAUTH_SVC_AP_READY:
                    vis = visible_rows(bs_ui_text_scale());
                    switch (nav) {
                        case BS_NAV_UP: case BS_NAV_PREV:
                            if (cursor > 0) cursor--;
                            scroll_clamp(cursor, &ap_scroll, vis);
                            dirty = true; break;
                        case BS_NAV_DOWN: case BS_NAV_NEXT:
                            if (cursor < deauth_svc_ap_count() - 1) cursor++;
                            scroll_clamp(cursor, &ap_scroll, vis);
                            dirty = true; break;
                        case BS_NAV_SELECT:
                            deauth_svc_ap_toggle(cursor);
                            dirty = true; break;
                        case BS_NAV_RIGHT: {
                            int sel = 0;
                            for (int i = 0; i < deauth_svc_ap_count(); i++)
                                if (deauth_svc_ap(i)->selected) sel++;
                            if (sel > 0) deauth_svc_sniff_clients();
                            break;
                        }
                        case BS_NAV_BACK:
                            deauth_svc_reset();
                            return;
                        default: break;
                    }
                    break;

                case DEAUTH_SVC_SNIFFING:
                    if (nav == BS_NAV_BACK) {
                        deauth_svc_sniff_skip();
                        dirty = true;
                    }
                    break;

                case DEAUTH_SVC_CLIENT_READY:
                    vis = visible_rows(bs_ui_text_scale());
                    switch (nav) {
                        case BS_NAV_UP: case BS_NAV_PREV:
                            if (cursor > 0) cursor--;
                            scroll_clamp(cursor, &cl_scroll, vis);
                            dirty = true; break;
                        case BS_NAV_DOWN: case BS_NAV_NEXT:
                            if (cursor < deauth_svc_client_count()) cursor++;
                            scroll_clamp(cursor, &cl_scroll, vis);
                            dirty = true; break;
                        case BS_NAV_SELECT:
                            if (cursor == 0)
                                deauth_svc_set_broadcast(!deauth_svc_broadcast_selected());
                            else
                                deauth_svc_client_toggle(cursor - 1);
                            dirty = true; break;
                        case BS_NAV_RIGHT: {
                            bool any = deauth_svc_broadcast_selected();
                            if (!any)
                                for (int i = 0; i < deauth_svc_client_count(); i++)
                                    if (deauth_svc_client(i)->selected) { any=true; break; }
                            if (any) deauth_svc_attack_start();
                            break;
                        }
                        case BS_NAV_BACK:
                            cursor = 0; ap_scroll = 0;
                            /* pop back to AP selection */
                            prev_state = DEAUTH_SVC_IDLE;  /* force redraw */
                            /* fake state by resetting scan results via reset+re-scan isn't ideal;
                               just restart full flow for simplicity */
                            deauth_svc_reset();
                            deauth_svc_scan_aps();
                            dirty = true; break;
                        default: break;
                    }
                    break;

                case DEAUTH_SVC_ATTACKING:
                    if (nav == BS_NAV_BACK) {
                        deauth_svc_stop();
                        dirty = true;
                    }
                    break;

                case DEAUTH_SVC_DONE:
                    if (nav == BS_NAV_SELECT) {
                        deauth_svc_reset();
                        deauth_svc_scan_aps();
                        cursor = 0; ap_scroll = 0; cl_scroll = 0;
                        dirty = true;
                    } else if (nav == BS_NAV_BACK) {
                        deauth_svc_reset();
                        return;
                    }
                    break;

                default: break;
            }
        }

        /* ── Tick service ── */
        deauth_svc_tick(now);
        state = deauth_svc_state();

        /* ── Time-gated dirty: rate depends on content type ── */
        {
            uint32_t interval = 0;
            switch (state) {
                case DEAUTH_SVC_SCANNING:     interval = k_scan_draw_ms;  break;
                case DEAUTH_SVC_SNIFFING:     interval = k_sniff_draw_ms; break;
                /* lists + attack log: 16 ms for smooth carousel */
                case DEAUTH_SVC_AP_READY:
                case DEAUTH_SVC_CLIENT_READY:
                case DEAUTH_SVC_ATTACKING:
                case DEAUTH_SVC_DONE:         interval = k_carousel_ms;   break;
                default: break;
            }
            if (interval && (now - last_draw_ms) >= interval)
                { dirty = true; last_draw_ms = now; }
        }

        /* ── Draw ── */
        if (dirty) {
            dirty = false;
            switch (state) {
                case DEAUTH_SVC_SCANNING:      draw_scan();                    break;
                case DEAUTH_SVC_AP_READY:      draw_ap_list(cursor, ap_scroll); break;
                case DEAUTH_SVC_SNIFFING:      draw_sniff();                   break;
                case DEAUTH_SVC_CLIENT_READY:  draw_client_list(cursor, cl_scroll); break;
                case DEAUTH_SVC_ATTACKING:     draw_attack();                  break;
                case DEAUTH_SVC_DONE:          draw_done();                    break;
                default: break;
            }
        }

        arch->delay_ms(1);
    }
}

#endif /* BS_HAS_WIFI */
