/*
 * ble_scan.c - BLE passive scanner.
 *
 * Maintains a deduplicated list of up to BLE_SCAN_MAX_DEVS devices.
 * Displays: MAC address, vendor tag, RSSI.
 * Scrollable with UP/DOWN; refreshes automatically on new or updated device.
 *
 * Navigation:
 *   UP / PREV  - scroll up
 *   DOWN / NEXT- scroll down
 *   BACK       - stop scan and return
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "apps/ble/ble_scan.h"
#include "apps/ble/ble_common.h"

#include "bs/bs_arch.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"

#include <string.h>
#include <stdio.h>

/* ── Device list ────────────────────────────────────────────────────────── */

#define BLE_SCAN_MAX_DEVS 32

typedef struct {
    uint8_t      addr[6];
    int8_t       rssi;
    ble_vendor_t vendor;
} scan_dev_t;

static scan_dev_t s_devs[BLE_SCAN_MAX_DEVS];
static int        s_dev_count  = 0;
static bool       s_scan_dirty = false;

static void scan_cb(const bs_ble_scan_result_t* r, void* ctx) {
    (void)ctx;

    /* Dedup: update RSSI if MAC already known */
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].addr, r->addr, 6) == 0) {
            s_devs[i].rssi = r->rssi;
            s_scan_dirty   = true;
            return;
        }
    }

    if (s_dev_count >= BLE_SCAN_MAX_DEVS) return;

    int idx = s_dev_count++;
    memcpy(s_devs[idx].addr, r->addr, 6);
    s_devs[idx].rssi   = r->rssi;
    s_devs[idx].vendor = ble_detect_vendor(r->adv.data, r->adv.len);
    s_scan_dirty = true;
}

/* ── Draw ───────────────────────────────────────────────────────────────── */

static void draw_scan(int scroll, int cursor) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 6;

    bs_gfx_clear(g_bs_theme.bg);

    char hdr[32];
    snprintf(hdr, sizeof(hdr), "BLE Scanner [%d]", s_dev_count);
    bs_ui_draw_header(hdr);

    if (s_dev_count == 0) {
        bs_gfx_text(8, cy, "Scanning...", g_bs_theme.dim, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        return;
    }

    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - cy - hint_h) / lh;
    if (visible < 1) visible = 1;

    for (int i = 0; i < visible && (scroll + i) < s_dev_count; i++) {
        int idx = scroll + i;
        bool sel = (idx == cursor);
        int  y   = cy + i * lh;

        if (sel)
            bs_gfx_fill_rect(0, y - 2, sw, lh - 1, g_bs_theme.dim);

        scan_dev_t* d = &s_devs[idx];

        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d->addr[0], d->addr[1], d->addr[2],
                 d->addr[3], d->addr[4], d->addr[5]);

        bs_color_t mc = sel ? g_bs_theme.accent  : g_bs_theme.primary;
        bs_color_t dc = sel ? g_bs_theme.primary : g_bs_theme.dim;

        bs_gfx_text(8, y, mac, mc, ts);

        char detail[32];
        snprintf(detail, sizeof(detail), "%s  %d dBm",
                 ble_vendor_str(d->vendor), (int)d->rssi);
        bs_gfx_text(8, y + bs_gfx_text_h(ts) + 2, detail, dc, ts2);
    }

    bs_ui_draw_hint("UP/DN=scroll  BACK=exit");
    bs_gfx_present();
}

/* ── Scroll helper ──────────────────────────────────────────────────────── */

static void update_scroll(int cursor, int* scroll) {
    int ts2     = bs_ui_text_scale() > 1 ? bs_ui_text_scale() - 1 : 1;
    int ts      = bs_ui_text_scale();
    int lh      = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 6;
    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - bs_ui_content_y() - hint_h) / lh;
    if (visible < 1) visible = 1;

    if (cursor < *scroll)              *scroll = cursor;
    if (cursor >= *scroll + visible)   *scroll = cursor - visible + 1;
    if (*scroll < 0)                   *scroll = 0;
}

/* ── Run ────────────────────────────────────────────────────────────────── */

void ble_scan_run(const bs_arch_t* arch) {
    memset(s_devs, 0, sizeof(s_devs));
    s_dev_count  = 0;
    s_scan_dirty = false;

    int  cursor = 0;
    int  scroll = 0;
    bool dirty  = true;

    bs_ble_scan_start(scan_cb, NULL);

    for (;;) {
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_UP: case BS_NAV_PREV:
                    if (cursor > 0) { cursor--; dirty = true; }
                    break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    if (cursor < s_dev_count - 1) { cursor++; dirty = true; }
                    break;
                case BS_NAV_BACK:
                    bs_ble_scan_stop();
                    return;
                default: break;
            }
        }

        if (s_scan_dirty) {
            s_scan_dirty = false;
            dirty = true;
        }

        if (dirty) {
            dirty = false;
            if (cursor >= s_dev_count && s_dev_count > 0)
                cursor = s_dev_count - 1;
            if (cursor < 0) cursor = 0;
            update_scroll(cursor, &scroll);
            draw_scan(scroll, cursor);
        }

        arch->delay_ms(10);
    }
}

#endif /* BS_HAS_BLE */
