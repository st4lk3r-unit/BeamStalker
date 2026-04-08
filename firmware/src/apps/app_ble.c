/*
 * app_ble.c - Bluetooth tools top-level menu.
 *
 * Sub-sections:
 *   [0] BLE Spam    — spoof vendor BLE advertisements (Apple/Samsung/Google)
 *   [1] BLE Scanner — passive scan, list devices with vendor tag + RSSI
 */
#include "bs/bs_ble.h"
#ifdef BS_HAS_BLE

#include "apps/app_ble.h"
#include "apps/ble/ble_spam.h"
#include "apps/ble/ble_scan.h"

#include "bs/bs_app.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"

#include <stdio.h>

/* ── BLE icon 16×16 1bpp ─────────────────────────────────────────────────
 *
 * Bluetooth symbol (ᛒ), stem at col 7, forks extend right:
 *
 *  col:    0123456789ABCDEF
 *  row  0: .......X........   stem top
 *  row  1: .......XX.......   upper fork right
 *  row  2: ........XX......
 *  row  3: .....X...X......   left crossing peak / right fork peak
 *  row  4: ......X..X......   diagonal converging back
 *  row  5: .......XX.......   centre crossover
 *  row  6: ......X..X......   lower fork start
 *  row  7: .....X...X......   lower fork peak / left crossing
 *  row  8: ........XX......
 *  row  9: .......XX.......   lower fork returns
 *  row 10: .......X........   stem bottom
 *  row 11-15: blank
 *
 * Byte layout: byte[0] MSB = col 0, byte[1] MSB = col 8.
 */
static const uint8_t k_ble_icon_16[] = {
    0x01, 0x00,  /* row  0:  .......X........ */
    0x01, 0x80,  /* row  1:  .......XX....... */
    0x00, 0xC0,  /* row  2:  ........XX...... */
    0x04, 0x40,  /* row  3:  .....X...X...... */
    0x02, 0x40,  /* row  4:  ......X..X...... */
    0x01, 0x80,  /* row  5:  .......XX....... */
    0x02, 0x40,  /* row  6:  ......X..X...... */
    0x04, 0x40,  /* row  7:  .....X...X...... */
    0x00, 0xC0,  /* row  8:  ........XX...... */
    0x01, 0x80,  /* row  9:  .......XX....... */
    0x01, 0x00,  /* row 10:  .......X........ */
    0x00, 0x00,  /* row 11 */
    0x00, 0x00,  /* row 12 */
    0x00, 0x00,  /* row 13 */
    0x00, 0x00,  /* row 14 */
    0x00, 0x00,  /* row 15 */
};

/* ── Sub-menu entries ────────────────────────────────────────────────────── */

typedef struct {
    const char* name;
    const char* desc;
    void (*run)(const bs_arch_t*);
} ble_entry_t;

static const ble_entry_t k_entries[] = {
    { "BLE Spam",    "Spoof Apple/Samsung/Google ads",  ble_spam_run },
    { "BLE Scanner", "Passive scan, list devices",      ble_scan_run },
};
#define BLE_ENTRY_COUNT (int)(sizeof(k_entries) / sizeof(k_entries[0]))

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_menu(int cursor, bool caps_ok) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 10;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("BLE Tools");

    if (!caps_ok) {
        bs_gfx_text(8, cy, "BLE unavailable on this target",
                    g_bs_theme.warn, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        return;
    }

    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - cy - hint_h) / lh;
    if (visible < 1) visible = 1;

    int scroll = cursor - visible / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > BLE_ENTRY_COUNT - visible) scroll = BLE_ENTRY_COUNT - visible;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < visible && (scroll + i) < BLE_ENTRY_COUNT; i++) {
        int idx = scroll + i;
        bool sel = (idx == cursor);
        int  y   = cy + i * lh;

        if (sel)
            bs_gfx_fill_rect(0, y - 3, sw, lh - 1, g_bs_theme.dim);

        bs_color_t nc = sel ? g_bs_theme.accent  : g_bs_theme.primary;
        bs_color_t dc = sel ? g_bs_theme.primary : g_bs_theme.dim;

        bs_gfx_text(8, y,                          k_entries[idx].name, nc, ts);
        bs_gfx_text(8, y + bs_gfx_text_h(ts) + 2, k_entries[idx].desc, dc, ts2);
    }

    bs_ui_draw_hint("SELECT=open  BACK=exit");
    bs_gfx_present();
}

/* ── App run ─────────────────────────────────────────────────────────────── */

static void app_ble_run(const bs_arch_t* arch) {
    int  cursor = 0;
    bool dirty  = true;

    int err = bs_ble_init(arch);
    bool caps_ok = (err == 0) &&
                   (bs_ble_caps() & (BS_BLE_CAP_ADVERTISE | BS_BLE_CAP_SCAN)) != 0;

    for (;;) {
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_UP:   case BS_NAV_PREV:
                    cursor = (cursor + BLE_ENTRY_COUNT - 1) % BLE_ENTRY_COUNT;
                    dirty = true; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    cursor = (cursor + 1) % BLE_ENTRY_COUNT;
                    dirty = true; break;
                case BS_NAV_SELECT:
                    if (caps_ok) {
                        bs_gfx_clear(g_bs_theme.bg);
                        k_entries[cursor].run(arch);
                        dirty = true;
                    }
                    break;
                case BS_NAV_BACK:
                    bs_ble_deinit();
                    return;
                default: break;
            }
        }

        if (dirty) {
            dirty = false;
            draw_menu(cursor, caps_ok);
        }

        arch->delay_ms(5);
    }
}

/* ── App descriptor ──────────────────────────────────────────────────────── */

const bs_app_t app_ble = {
    .name   = "BLE",
    .icon   = k_ble_icon_16,
    .icon_w = 16,
    .icon_h = 16,
    .run    = app_ble_run,
};

#endif /* BS_HAS_BLE */
