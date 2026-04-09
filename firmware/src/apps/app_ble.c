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
    float ts = bs_ui_text_scale();
    int   cy = bs_ui_content_y();
    int   lh = bs_ui_menu_row_h(ts);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("BLE Tools");

    if (!caps_ok) {
        bs_gfx_text(8, cy, "BLE unavailable on this target", g_bs_theme.warn, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        return;
    }

    int visible = bs_ui_menu_visible(ts);
    int scroll  = cursor - visible / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > BLE_ENTRY_COUNT - visible) scroll = BLE_ENTRY_COUNT - visible;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < visible && (scroll + i) < BLE_ENTRY_COUNT; i++) {
        int idx = scroll + i;
        bs_ui_draw_menu_row(cy + i * lh, k_entries[idx].name, k_entries[idx].desc,
                            idx == cursor, ts);
    }

    bs_ui_draw_scroll_arrows(scroll, BLE_ENTRY_COUNT, visible);
    bs_ui_draw_hint("SELECT=open  BACK=exit");
    bs_gfx_present();
}

/* ── App run ─────────────────────────────────────────────────────────────── */

static void app_ble_run(const bs_arch_t* arch) {
    int  cursor = 0;

    int err = bs_ble_init(arch);
    bool caps_ok = (err == 0) &&
                   (bs_ble_caps() & (BS_BLE_CAP_ADVERTISE | BS_BLE_CAP_SCAN)) != 0;

    bool dirty = true;
    uint32_t prev_ms = arch->millis();
    uint32_t last_anim_ms = prev_ms;
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_UP:   case BS_NAV_PREV:
                    cursor = (cursor + BLE_ENTRY_COUNT - 1) % BLE_ENTRY_COUNT; dirty = true; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    cursor = (cursor + 1) % BLE_ENTRY_COUNT; dirty = true; break;
                case BS_NAV_SELECT:
                    if (caps_ok) {
                        bs_gfx_clear(g_bs_theme.bg);
                        k_entries[cursor].run(arch);
                        dirty = true;
                        last_anim_ms = arch->millis();
                    }
                    break;
                case BS_NAV_BACK:
                    bs_ble_deinit();
                    return;
                default: break;
            }
        }

        bool anim_due = bs_ui_carousel_enabled() && (uint32_t)(now - last_anim_ms) >= 100U;
        if (dirty || anim_due) {
            draw_menu(cursor, caps_ok);
            dirty = false;
            if (anim_due) last_anim_ms = now;
        }
#if defined(VARIANT_TPAGER)
        arch->delay_ms(1);
#else
        arch->delay_ms(2);
#endif
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
