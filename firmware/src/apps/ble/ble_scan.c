/*
 * ble_scan.c - BLE scanner UI (thin wrapper over ble_scan_svc).
 *
 * All scan logic lives in ble_scan_svc.c.
 * This file owns: draw, scroll, input.
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "apps/ble/ble_scan.h"
#include "apps/ble/ble_scan_svc.h"
#include "apps/ble/ble_common.h"

#include "bs/bs_arch.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"

#include <stdio.h>

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_scan(int scroll, int cursor) {
    float ts  = bs_ui_text_scale();
    int   cy  = bs_ui_content_y();
    int   lh  = bs_ui_menu_row_h(ts);
    int   cnt = ble_scan_svc_count();

    bs_gfx_clear(g_bs_theme.bg);

    char hdr[32];
    snprintf(hdr, sizeof hdr, "BLE Scanner [%d]", cnt);
    bs_ui_draw_header(hdr);

    if (cnt == 0) {
        bs_gfx_text(8, cy, "Scanning...", g_bs_theme.dim, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        return;
    }

    int visible = bs_ui_menu_visible(ts);

    for (int i = 0; i < visible && (scroll + i) < cnt; i++) {
        int idx = scroll + i;
        const ble_scan_dev_t* d = ble_scan_svc_dev(idx);
        if (!d) continue;

        char mac[18];
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5]);

        char detail[32];
        snprintf(detail, sizeof detail, "%s  %d dBm",
                 ble_vendor_str(d->vendor), (int)d->rssi);

        bs_ui_draw_menu_row(cy + i * lh, mac, detail, idx == cursor, ts);
    }

    bs_ui_draw_scroll_arrows(scroll, cnt, visible);
    bs_ui_draw_hint("UP/DN=scroll  BACK=exit");
    bs_gfx_present();
}

/* ── Scroll helper ───────────────────────────────────────────────────────── */

static void update_scroll(int cursor, int* scroll) {
    int visible = bs_ui_menu_visible(bs_ui_text_scale());
    bs_ui_list_clamp_scroll(cursor, scroll, ble_scan_svc_count(), visible);
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void ble_scan_run(const bs_arch_t* arch) {
    ble_scan_svc_init(arch);
    ble_scan_svc_start();

    int cursor = 0, scroll = 0;

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_UP: case BS_NAV_PREV:
                    if (cursor > 0) cursor--; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    if (cursor < ble_scan_svc_count() - 1) cursor++; break;
                case BS_NAV_BACK:
                    ble_scan_svc_stop();
                    return;
                default: break;
            }
        }

        ble_scan_svc_clear_dirty();

        int cnt = ble_scan_svc_count();
        if (cursor >= cnt && cnt > 0) cursor = cnt - 1;
        if (cursor < 0) cursor = 0;
        update_scroll(cursor, &scroll);
        draw_scan(scroll, cursor);   /* always redraw — carousel needs it */

        arch->delay_ms(16);
    }
}

#endif /* BS_HAS_BLE */
