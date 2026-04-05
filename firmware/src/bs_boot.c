/*
 * bs_boot.c - Fallout-flavored boot sequence.
 *
 * Layout (proportional to screen size):
 *
 *   ┌──────────────────────────────────────────────┐
 *   │                                              │ ← top padding
 *   │  ██████╗ ███████╗ █████╗ ███╗  ███╗          │
 *   │  ██╔══██╗██╔════╝██╔══██╗████╗████║ STALKER  │ ← banner (5×7 scaled)
 *   │  ██████╔╝█████╗  ███████║██╔████╔██║         │
 *   │  ██╔══██╗██╔══╝  ██╔══██║██║╚██╔╝██║         │
 *   │  ██████╔╝███████╗██║  ██║██║ ╚═╝ ██║         │
 *   │  ╚═════╝ ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝         │
 *   │         v 0.1.0  ·  a cross-platform fw       │
 *   │──────────────────────────────────────────────│ ← separator
 *   │  [  OK  ] arch: POSIX native                 │
 *   │  [  OK  ] console: 115200 baud               │
 *   │  [  OK  ] display: 480×222 ST7796            │
 *   │  [  OK  ] keyboard: TCA8418 64-key           │
 *   │  [ WARN ] audio: not configured              │
 *   │  [  OK  ] BeamStalker ready                  │
 *   └──────────────────────────────────────────────┘
 *
 * On native: "BEAM" drawn with scale=1 block chars (5×7 per glyph = 5 rows,
 * 66 cols for 11 chars), "STALKER" printed inline as plain text.
 * On SGFX: "BEAM" drawn with scale=3, "STALKER" printed at scale=2 offset.
 */
#include "bs_boot.h"
#include "bs/bs_gfx.h"
#include "bs/bs_keys.h"
#include "bs/bs_log.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_fs.h"
#include "bs/bs_wifi.h"
#include "bs/bs_ble.h"
#include <stdio.h>
#include <string.h>
#include "bs/bs_assets.h"

/* ---- Layout helpers --------------------------------------------------- */

/*
 * Draw the two-part banner:
 *   "BEAM"    - large block text (scale)
 *   "STALKER" - same scale, offset to the right
 * Both rendered on the same baseline row.
 */
static void draw_banner(const bs_arch_t* arch) {
    int sw = bs_gfx_width();

    /* Skull: 120×120 source rendered at step=2 → 60×60 on screen */
    int skull_x = 4,  skull_y = 4;
    int skull_w = 60, skull_h = 60;
    int sep_y   = skull_y + skull_h + 4;   /* guaranteed below skull: 4+60+4=68 */

    /* Clear banner region (exactly to separator, never more) */
    bs_gfx_fill_rect(0, 0, sw, sep_y + 1, g_bs_theme.bg);

    /* Skull logo - top-left, downscaled 2× */
    bs_gfx_bitmap_1bpp(skull_x, skull_y, 120, 120, bs_skull_120, g_bs_theme.primary, 1, 2);

    /* "BEAM STALKER" - right of skull, text block vertically centred in skull height */
    int sc      = 2;
    int th      = bs_gfx_text_h(sc);                       /* 14 px */
    int h1      = bs_gfx_text_h(1);                        /*  7 px */
    int block_h = th + 3 + h1;                             /* 24 px */
    int tx      = skull_x + skull_w + 10;                  /* 74    */
    int ty      = skull_y + (skull_h - block_h) / 2;       /* 22    */
    int bw      = bs_gfx_text_w("BEAM", sc);
    int sp      = bs_gfx_text_w(" ", sc);
    bs_gfx_text(tx,        ty, "BEAM",    g_bs_theme.primary,     sc);
    bs_gfx_text(tx+bw+sp,  ty, "STALKER", g_bs_theme.accent, sc);

    /* Version tag */
    int vy = ty + th + 3;
    const char* ver = "v" BS_VERSION "  ·  cross-platform firmware";
    bs_gfx_text(tx, vy, ver, g_bs_theme.dim, 1);

    /* Separator - anchored to skull bottom, not text bottom */
    bs_gfx_hline(0, sep_y, sw, g_bs_theme.dim);

    bs_log_boot_reset(sep_y + 3);
    bs_gfx_present();
    (void)arch;
}

/* ---- Boot sequence ----------------------------------------------------- */

void bs_boot_run(const bs_arch_t* arch, void (*idle_fn)(void)) {
    /* Blank screen */
    bs_gfx_clear(g_bs_theme.bg);
    bs_gfx_present();

    /* Draw static banner + separator */
    draw_banner(arch);

    /* Give the banner a moment to breathe */
    arch->delay_ms(300);

    /* --- Component initialisation log entries --------------------------- */

    /* arch */
#if defined(VARIANT_TPAGER)
    BS_LOGOK("arch", "ESP32-S3 arduino - T-Pager");
#elif defined(VARIANT_CARDPUTER)
    BS_LOGOK("arch", "ESP32-S3 arduino - Cardputer");
#elif defined(VARIANT_NATIVE)
    BS_LOGOK("arch", "POSIX native linux");
#else
    BS_LOGOK("arch", "unknown variant");
#endif
    arch->delay_ms(80);

    /* console */
#ifdef BS_UART_BAUD
    {
        char buf[48];
        snprintf(buf, sizeof buf, "%u baud", (unsigned)BS_UART_BAUD);
        BS_LOGOK("console", buf);
    }
#else
    BS_LOGOK("console", "115200 baud");
#endif
    arch->delay_ms(60);

    /* display */
#ifdef BS_USE_SGFX
    {
        char buf[48];
        snprintf(buf, sizeof buf, "%dx%d hardware display", bs_gfx_width(), bs_gfx_height());
        BS_LOGOK("display", buf);
    }
#elif defined(BS_GFX_NATIVE)
    {
        char buf[48];
        snprintf(buf, sizeof buf, "native terminal %dx%d", bs_gfx_width(), bs_gfx_height());
        BS_LOGOK("display", buf);
    }
#else
    BS_LOGBW("display", "no display configured");
#endif
    arch->delay_ms(80);

    /* keyboard */
#ifdef BS_KEYS_SIC
#  if defined(VARIANT_CARDPUTER)
    BS_LOGOK("keyboard", "74HC138 56-key matrix");
    arch->delay_ms(120);
#  else
    BS_LOGOK("keyboard", "TCA8418 64-key matrix");
    arch->delay_ms(120);
    BS_LOGOK("encoder", "rotary input active");
    arch->delay_ms(60);
#  endif
#elif defined(BS_KEYS_NATIVE)
    BS_LOGOK("keyboard", "raw terminal input");
    arch->delay_ms(60);
#endif

    /* audio (SIC codec - future) */
#ifdef BS_USE_SIC
    BS_LOGBW("audio", "SIC loaded - codec probe pending");
#else
    BS_LOGBW("audio", "not configured");
#endif
    arch->delay_ms(80);

    /* filesystem */
    if (bs_fs_available()) {
        BS_LOGOK("storage", "SD card mounted");
    } else {
        BS_LOGBW("storage", "no SD card");
    }
    arch->delay_ms(60);

/* WiFi sanity probe: init → read caps → stop (driver stays resident) */
#ifdef BS_HAS_WIFI
    {
        int werr = bs_wifi_init(arch);
        if (werr == 0) {
            uint32_t wcaps = bs_wifi_caps();
            BS_LOGOK("wifi", "caps=0x%02X  inject=%s sniff=%s scan=%s",
                     (unsigned)wcaps,
                     (wcaps & BS_WIFI_CAP_INJECT) ? "Y" : "N",
                     (wcaps & BS_WIFI_CAP_SNIFF)  ? "Y" : "N",
                     (wcaps & BS_WIFI_CAP_SCAN)   ? "Y" : "N");
            bs_wifi_deinit();
        } else {
            BS_LOGBF("wifi", "init failed (%d)", werr);
        }
    }
    arch->delay_ms(60);
#endif

/* BLE sanity probe: init → read caps → deinit (controller stays resident) */
#ifdef BS_HAS_BLE
    {
        int berr = bs_ble_init(arch);
        if (berr == 0) {
            uint32_t bcaps = bs_ble_caps();
            BS_LOGOK("ble", "caps=0x%02X  adv=%s scan=%s rand_addr=%s",
                     (unsigned)bcaps,
                     (bcaps & BS_BLE_CAP_ADVERTISE)  ? "Y" : "N",
                     (bcaps & BS_BLE_CAP_SCAN)        ? "Y" : "N",
                     (bcaps & BS_BLE_CAP_RAND_ADDR)   ? "Y" : "N");
            bs_ble_deinit();
        } else {
            BS_LOGBF("ble", "init failed (%d)", berr);
        }
    }
    arch->delay_ms(60);
#endif

    /* final ready */
    arch->delay_ms(200);
    BS_LOGOK("system", "BeamStalker ready");
    arch->delay_ms(500);

    /* Press any key on the UI display - terminal konsole kept alive via idle_fn */
    {
        static const char* prompt = "[ press any key ]";
        int ts   = bs_ui_text_scale();
        int sw   = bs_gfx_width();
        int sh   = bs_gfx_height();
        int pw   = bs_gfx_text_w(prompt, ts);
        int px   = (sw - pw) / 2;
        int py   = sh - bs_gfx_text_h(ts) - 4;
        int blink_ctr = 0;
        int visible   = 1;

        bs_gfx_text(px, py, prompt, g_bs_theme.dim, ts);
        bs_gfx_present();

        for (;;) {
            bs_key_t key;
            if (bs_keys_poll(&key) && key.id != BS_KEY_NONE) break;

            if (idle_fn) idle_fn();
            arch->delay_ms(30);

            blink_ctr++;
            if (blink_ctr >= 17) {
                blink_ctr = 0;
                visible   = !visible;
                bs_gfx_fill_rect(px, py, pw, bs_gfx_text_h(ts), g_bs_theme.bg);
                if (visible)
                    bs_gfx_text(px, py, prompt, g_bs_theme.dim, ts);
                bs_gfx_present();
            }
        }

        bs_gfx_fill_rect(px, py, pw, bs_gfx_text_h(ts), g_bs_theme.bg);
        bs_gfx_present();
    }
}
