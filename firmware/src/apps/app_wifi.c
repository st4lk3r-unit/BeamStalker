/*
 * app_wifi.c - WiFi tools top-level menu.
 *
 * Sub-sections:
 *   [0] Beacon Spam  — flood the air with 802.11 beacons
 *   [1] Deauther     — deauth clients from selected APs
 *   [2] Sniffer      — promiscuous capture to PCAP on SD
 */
#include "bs/bs_wifi.h"   /* must come first — defines BS_HAS_WIFI */
#ifdef BS_HAS_WIFI

#include "apps/app_wifi.h"
#include "apps/wifi/wifi_beacon.h"
#include "apps/wifi/wifi_deauth.h"
#include "apps/wifi/wifi_sniffer.h"
#include "apps/wifi/wifi_honeypot.h"
#include "apps/wifi/wifi_karma.h"
#include "apps/wifi/wifi_captive.h"
#include "apps/wifi/wifi_eviltwin.h"

#include "bs/bs_app.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"

#include <stdio.h>

/* ── WiFi signal icon 16×16 1bpp ─────────────────────────────────────────── */
/*
 *  Row 0:  ................  (blank)
 *  Row 1:  ....XXXXXXXXX...  outer arc top
 *  Row 2:  ...X.........X..
 *  Row 3:  ..X...........X.
 *  Row 4:  ................
 *  Row 5:  .....XXXXXXX....  middle arc
 *  Row 6:  ....X.......X...
 *  Row 7:  ................
 *  Row 8:  ......XXXXX.....  inner arc
 *  Row 9:  .....X.....X....
 *  Row10:  ................
 *  Row11:  ........X.......  dot
 *  Row12:  ........X.......
 *  Row13:  ................
 *  Row14:  ................
 *  Row15:  ................
 */
static const uint8_t k_wifi_icon_16[] = {
    0x00, 0x00,  /* row  0: blank                     */
    0x0F, 0xF8,  /* row  1: ....XXXXXXXXX...           */
    0x10, 0x04,  /* row  2: ...X.........X..           */
    0x20, 0x02,  /* row  3: ..X...........X.           */
    0x00, 0x00,  /* row  4: blank                     */
    0x07, 0xF0,  /* row  5: .....XXXXXXX....           */
    0x08, 0x08,  /* row  6: ....X.......X...           */
    0x00, 0x00,  /* row  7: blank                     */
    0x03, 0xE0,  /* row  8: ......XXXXX.....           */
    0x04, 0x10,  /* row  9: .....X.....X....           */
    0x00, 0x00,  /* row 10: blank                     */
    0x00, 0x80,  /* row 11: ........X.......  dot      */
    0x00, 0x80,  /* row 12: ........X.......           */
    0x00, 0x00,  /* row 13: blank                     */
    0x00, 0x00,  /* row 14: blank                     */
    0x00, 0x00,  /* row 15: blank                     */
};

/* ── Sub-menu entries ────────────────────────────────────────────────────── */

typedef struct {
    const char* name;
    const char* desc;
    void (*run)(const bs_arch_t*);
} wifi_entry_t;

static const wifi_entry_t k_entries[] = {
    { "Beacon Spam", "Flood with fake 802.11 beacons",  wifi_beacon_run   },
    { "Deauther",    "Kick clients off their APs",      wifi_deauth_run   },
    { "Sniffer",     "Capture 802.11 frames to PCAP",   wifi_sniffer_run  },
    { "Honeypot",    "Clone AP; serve captive portal",   wifi_honeypot_run },
    { "Karma",       "Mirror probe SSIDs; lure clients", wifi_karma_run    },
    { "Evil Twin",   "Clone AP + deauth + portal",       wifi_eviltwin_run },
    { "Cap. Portal", "Manual rogue AP + portal",         wifi_captive_run  },
};
#define WIFI_ENTRY_COUNT (int)(sizeof(k_entries)/sizeof(k_entries[0]))

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_menu(int cursor, bool caps_ok) {
    float ts = bs_ui_text_scale();
    int   cy = bs_ui_content_y();
    int   lh = bs_ui_menu_row_h(ts);

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("WiFi Tools");

    if (!caps_ok) {
        bs_gfx_text(8, cy, "WiFi unavailable on this target", g_bs_theme.warn, ts);
        bs_ui_draw_hint("BACK=exit");
        bs_gfx_present();
        return;
    }

    int visible = bs_ui_menu_visible(ts);
    int scroll  = cursor - visible / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > WIFI_ENTRY_COUNT - visible) scroll = WIFI_ENTRY_COUNT - visible;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < visible && (scroll + i) < WIFI_ENTRY_COUNT; i++) {
        int idx = scroll + i;
        bs_ui_draw_menu_row(cy + i * lh, k_entries[idx].name, k_entries[idx].desc,
                            idx == cursor, ts);
    }

    bs_ui_draw_scroll_arrows(scroll, WIFI_ENTRY_COUNT, visible);
    bs_ui_draw_hint("SELECT=open  BACK=exit");
    bs_gfx_present();
}

/* ── App run ─────────────────────────────────────────────────────────────── */

static void app_wifi_run(const bs_arch_t* arch) {
    int  cursor  = 0;

    int err = bs_wifi_init(arch);
    bool caps_ok = (err == 0) &&
                   (bs_wifi_caps() & (BS_WIFI_CAP_INJECT | BS_WIFI_CAP_SNIFF)) != 0;

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_UP:   case BS_NAV_PREV:
                    cursor = (cursor + WIFI_ENTRY_COUNT - 1) % WIFI_ENTRY_COUNT; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    cursor = (cursor + 1) % WIFI_ENTRY_COUNT; break;
                case BS_NAV_SELECT:
                    if (caps_ok) {
                        bs_gfx_clear(g_bs_theme.bg);
                        k_entries[cursor].run(arch);
                    }
                    break;
                case BS_NAV_BACK:
                    bs_wifi_deinit();
                    return;
                default: break;
            }
        }

        draw_menu(cursor, caps_ok);
        arch->delay_ms(16);
    }
}

/* ── App descriptor ──────────────────────────────────────────────────────── */

const bs_app_t app_wifi = {
    .name   = "WiFi",
    .icon   = k_wifi_icon_16,
    .icon_w = 16,
    .icon_h = 16,
    .run    = app_wifi_run,
};

#endif /* BS_HAS_WIFI */
