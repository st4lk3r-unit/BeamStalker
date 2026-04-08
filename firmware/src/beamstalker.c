/*
 * beamstalker.c - core firmware.
 *
 * Init sequence:
 *   1. arch vtable (timing, UART)
 *   2. bs_gfx (display backend - SDL2 window on native, SGFX on hardware,
 *              no-op when BS_HEADLESS / neither backend defined)
 *   3. bs_keys (input backend - SDL2 events on native, SIC on hardware)
 *   4. konsole (UART CLI; always on UART/stdin+stdout)
 *   5. bs_log (routes to konsole + display)
 *   6. bs_debug (FPS/heap overlay)
 *   7. SIC hardware (keyboard, audio, battery … - hardware targets only)
 *   8. bs_boot_run()   - animated boot sequence         [skipped if BS_HEADLESS]
 *   9. bs_theme_set()  - apply default theme
 *  10. bs_menu_init()  - set up app menu                [skipped if BS_HEADLESS]
 *
 * Run loop (bs_run, called forever from main):
 *   Normal:   bs_menu_run() → app->run() → bs_menu_invalidate()
 *   Headless: konsole_poll() only — use 'startapp <name>' to launch apps
 *
 * Build flags:
 *   BS_HEADLESS      - skip menu/boot, poll konsole only
 *   BS_NO_APP_DVD    - exclude DVD app
 *   BS_NO_APP_LOG    - exclude Log app
 *   BS_NO_APP_TOP    - exclude Top app
 *   BS_NO_APP_WIFI   - exclude WiFi app (auto-excluded if no WiFi backend)
 *   (app_settings is always included)
 */
#include "beamstalker.h"
#include "bs/bs_arch.h"
#include "bs/bs_gfx.h"
#include "bs/bs_keys.h"
#include "bs/bs_log.h"
#include "bs/bs_debug.h"
#include "bs/bs_hw.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_app.h"
#include "bs/bs_menu.h"
#include "bs/bs_fs.h"
#include "bs/bs_ui.h"
#include "bs_boot.h"

#include "apps/app_settings.h"
#ifndef BS_NO_APP_DVD
#  include "apps/app_dvd.h"
#endif
#ifndef BS_NO_APP_LOG
#  include "apps/app_log.h"
#endif
#ifndef BS_NO_APP_TOP
#  include "apps/app_top.h"
#endif
#include "bs/bs_wifi.h"   /* defines BS_HAS_WIFI when a backend is active */
#if defined(BS_HAS_WIFI) && !defined(BS_NO_APP_WIFI)
#  include "apps/app_wifi.h"
#endif
#ifdef BS_HAS_WIFI
#  include "apps/wifi/wifi_deauth_svc.h"
#  include "apps/wifi/wifi_beacon_svc.h"
#  include "apps/wifi/wifi_sniffer_svc.h"
#endif
#include "bs/bs_ble.h"    /* defines BS_HAS_BLE when a backend is active */
#if defined(BS_HAS_BLE) && !defined(BS_NO_APP_BLE)
#  include "apps/app_ble.h"
#endif
#ifdef BS_HAS_BLE
#  include "apps/ble/ble_spam_svc.h"
#  include "apps/ble/ble_scan_svc.h"
#  include "apps/ble/ble_common.h"   /* k_ble_spam_mode_names */
#endif

#include "konsole/konsole.h"
#include "konsole/static.h"   /* struct kon_line_state */
#include "board.h"            /* variant-specific defines (via -I variant/<name>) */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Optional SIC ----------------------------------------------------- */
#ifdef BS_USE_SIC
#  include <sic/sic.h>
#  include <sic/input/kscan.h>
#endif

/* ---- ESP log hook (captures [E][sd_diskio...] into bs_log ring) -------- */
#ifdef ARCH_ESP32
extern void bs_log_esp_hook_init(void);
#endif

/* ---- Konsole instance + storage --------------------------------------- */
static struct konsole        g_ks;
static struct kon_line_state g_ks_line;

/* ---- konsole_io callbacks -------------------------------------------- */
static const bs_arch_t* s_arch = NULL;

static size_t io_read_avail(void* ctx) { (void)ctx; return 1024; }

static size_t io_read(void* ctx, uint8_t* buf, size_t len) {
    (void)ctx;
    int n = s_arch->uart_read(0, buf, len);
    return (size_t)(n < 0 ? 0 : n);
}

static size_t io_write(void* ctx, const uint8_t* buf, size_t len) {
    (void)ctx;
    int w = s_arch->uart_write(0, buf, len);
    return (size_t)(w < 0 ? 0 : w);
}

static uint32_t io_millis(void* ctx) { (void)ctx; return s_arch->millis(); }

/* ---- Forward declaration (attack tick, defined below) ----------------- */
static void hl_tick(void);

/* ---- Idle callbacks --------------------------------------------------- */
static void menu_idle_poll(void) {
    konsole_poll(&g_ks);
    hl_tick();   /* drive any CLI-started attack on non-headless targets too */
}

static void boot_idle_poll(void) {
    /* Print prompt once (after boot log), then keep konsole responsive */
    static bool s_prompt_shown = false;
    if (!s_prompt_shown) {
        kon_banner(&g_ks, NULL);
        s_prompt_shown = true;
    }
    konsole_poll(&g_ks);
}

/* ---- App list --------------------------------------------------------- */
static const bs_app_t* const k_apps[] = {
#if defined(BS_HAS_WIFI) && !defined(BS_NO_APP_WIFI)
    &app_wifi,
#endif
#if defined(BS_HAS_BLE) && !defined(BS_NO_APP_BLE)
    &app_ble,
#endif
#ifndef BS_NO_APP_LOG
    &app_log,
#endif
#ifndef BS_NO_APP_DVD
    &app_dvd,
#endif
    &app_settings,
#ifndef BS_NO_APP_TOP
    &app_top,
#endif
};
static const int k_n_apps = (int)(sizeof(k_apps) / sizeof(k_apps[0]));

/* ---- Commands --------------------------------------------------------- */

static int cmd_help(struct konsole* ks, int argc, char** argv) {
    (void)argc; (void)argv; kon_print_help(ks); return 0;
}

static int cmd_hw(struct konsole* ks, int argc, char** argv) {
    (void)argc; (void)argv;
    bs_hw_info_t h;
    bs_hw_get_info(&h);

    kon_printf(ks, "\033[38;2;255;170;34m[hw]\033[0m %s\r\n", h.board ? h.board : "?");
    if (h.cores > 0)
        kon_printf(ks, "  Chip  : rev%d, %d core%s\r\n",
                   h.chip_rev, h.cores, h.cores > 1 ? "s" : "");
    if (h.cpu_mhz)
        kon_printf(ks, "  CPU   : %u MHz\r\n", h.cpu_mhz);
    if (h.flash_kb)
        kon_printf(ks, "  Flash : %u kB\r\n", h.flash_kb);
    if (h.heap_free_kb || h.heap_min_kb)
        kon_printf(ks, "  Heap  : %u kB free  (min ever %u kB)\r\n",
                   h.heap_free_kb, h.heap_min_kb);
    if (h.psram_total_kb)
        kon_printf(ks, "  PSRAM : %u kB free / %u kB total\r\n",
                   h.psram_free_kb, h.psram_total_kb);
    if (h.sdk_ver)
        kon_printf(ks, "  SDK   : %s\r\n", h.sdk_ver);

    /* Display */
#ifdef BS_HEADLESS
    kon_printf(ks, "  Disp  : headless (no display)\r\n");
#else
    kon_printf(ks, "  Disp  : %dx%d  [  OK  ]\r\n", bs_gfx_width(), bs_gfx_height());
#endif

    /* Battery */
    {
        int pct = bs_hw_battery_pct();
        int mv  = bs_hw_battery_mv();
        if (pct > 0 || mv > 0)
            kon_printf(ks, "  Bat   : %d%%  %d.%02dV  [  OK  ]\r\n",
                       pct, mv / 1000, (mv % 1000) / 10);
        else
#ifdef BS_USE_SIC
            kon_printf(ks, "  Bat   : [ ERR ] gauge not responding\r\n");
#else
            kon_printf(ks, "  Bat   : not configured\r\n");
#endif
    }

    /* Storage */
    if (bs_fs_available()) {
        long log_sz = bs_fs_file_size("system.log");
        if (log_sz >= 0)
            kon_printf(ks, "  SD    : mounted  log %ld B  [  OK  ]\r\n", log_sz);
        else
            kon_printf(ks, "  SD    : mounted  [  OK  ]\r\n");
    } else {
#ifdef BS_FS_SDCARD
        const char* sd_err = bs_fs_init_error();
        kon_printf(ks, "  SD    : [ ERR ] %s\r\n",
                   sd_err ? sd_err : "not mounted");
        kon_printf(ks, "          pins CS=%d SCK=%d MOSI=%d MISO=%d\r\n",
                   BS_SD_CS_PIN, BS_SD_SCK_PIN, BS_SD_MOSI_PIN, BS_SD_MISO_PIN);
#else
        kon_printf(ks, "  SD    : not configured\r\n");
#endif
    }

#if defined(VARIANT_TPAGER)
    /* XL9555 GPIO expander diagnostic */
    {
        bs_hw_xl9555_t xl;
        bs_hw_xl9555_read(&xl);
        if (!xl.reachable) {
            kon_printf(ks, "  XL9555: [ ERR ] I2C 0x20 not responding\r\n");
        } else {
            bool sd_det = !(xl.input_1  & (1 << 2)); /* P12 active-LOW */
            bool sd_en  =  (xl.output_1 & (1 << 4)); /* P14 HIGH = powered */
            bool sd_out =  !(xl.config_1 & (1 << 4)); /* P14 configured as output */
            kon_printf(ks, "  XL9555: INPUT_1=0x%02X  CFG_1=0x%02X  OUT_1=0x%02X\r\n",
                       xl.input_1, xl.config_1, xl.output_1);
            kon_printf(ks, "          SD_DET(P12)=%s  SD_EN(P14)=%s(%s)\r\n",
                       sd_det ? "card" : "NO CARD",
                       sd_out ? (sd_en ? "HIGH" : "LOW") : "INPUT!",
                       sd_out ? "out" : "BUG:in");
        }
    }
#endif

    kon_printf(ks, "  Up    : %.1f s\r\n", (float)s_arch->millis() / 1000.0f);
    return 0;
}

static int cmd_dmesg(struct konsole* ks, int argc, char** argv) {
    /* dmesg         - print in-memory log (boot to now)
     * dmesg flush   - also write current ring to SD system.log */
    bool do_flush = (argc >= 2 && strcmp(argv[1], "flush") == 0);

    bs_log_print_all(ks);

    if (do_flush) {
        bs_log_flush_sd();
        if (bs_fs_available())
            kon_printf(ks, "--- flushed to system.log ---\r\n");
        else
            kon_printf(ks, "--- no storage, flush skipped ---\r\n");
    }
    return 0;
}

static int cmd_sdformat(struct konsole* ks, int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        kon_printf(ks, "WARNING: erases ALL data on the SD card.\r\n");
        kon_printf(ks, "Run 'sdformat yes' to confirm.\r\n");
        return 0;
    }
    kon_printf(ks, "Formatting SD card as FAT32, please wait...\r\n");
    if (bs_fs_format() == 0) {
        bs_log_flush_sd();   /* write boot log now that SD is up */
        kon_printf(ks, "Done - SD card formatted and mounted.\r\n");
    } else {
        const char* e = bs_fs_init_error();
        kon_printf(ks, "Failed: %s\r\n", e ? e : "unknown error");
    }
    return 0;
}

/* Case-insensitive string compare (POSIX strcasecmp may not be available) */
static int bs_stricmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}


static int cmd_startapp(struct konsole* ks, int argc, char** argv) {
    /* startapp          - show usage
     * startapp --list   - list registered apps
     * startapp <name>   - launch app by name (case-insensitive) */
    if (argc < 2 || strcmp(argv[1], "--list") == 0) {
        if (argc < 2)
            kon_printf(ks, "Usage: startapp --list | startapp <name>\r\n");
        kon_printf(ks, "Apps (%d):\r\n", k_n_apps);
        for (int i = 0; i < k_n_apps; i++)
            kon_printf(ks, "  %s\r\n", k_apps[i]->name ? k_apps[i]->name : "?");
        return 0;
    }

    for (int i = 0; i < k_n_apps; i++) {
        if (k_apps[i]->name && bs_stricmp(k_apps[i]->name, argv[1]) == 0) {
            if (!k_apps[i]->run) {
                kon_printf(ks, "app '%s' has no run function\r\n", argv[1]);
                return 1;
            }
#ifndef BS_HEADLESS
            bs_gfx_clear(g_bs_theme.bg);
#endif
            k_apps[i]->run(s_arch);
#ifndef BS_HEADLESS
            bs_gfx_clear(g_bs_theme.bg);
            bs_menu_invalidate();
#endif
            return 0;
        }
    }

    kon_printf(ks, "unknown app '%s' — try 'startapp --list'\r\n", argv[1]);
    return 1;
}

/* ---- Attack command engine -------------------------------------------- */
/*
 * CLI-controlled background attacks.  The same service layer used by the
 * UI apps is driven here, so there is zero duplicated attack logic.
 * hl_tick() is called from both menu_idle_poll (non-headless) and the
 * headless bs_run() loop — commands work on every build target.
 *
 * Commands:
 *   wifi                                        - show WiFi/attack status
 *   wifi beacon start [--mode random|custom]
 *                     [--prefix P] [--charset ascii|hi|kata|cyr] [--ch N]
 *   wifi beacon stop
 *   wifi deauth  start [--ch N]                 - broadcast flood
 *   wifi deauth  stop
 *   wifi sniff   start [--ch N]                 - passive capture, auto-hop if no --ch
 *   wifi sniff   stop
 *   ble spam start [--mode apple|samsung|google|name|all] [--ivl ms]
 *   ble spam stop
 *   ble scan start
 *   ble scan stop
 */

typedef enum {
    HL_IDLE = 0,
    HL_WIFI_BEACON,
    HL_WIFI_DEAUTH,
    HL_WIFI_SNIFF,
    HL_BLE_SPAM,
    HL_BLE_SCAN,
} hl_mode_t;

static hl_mode_t       s_hl_mode        = HL_IDLE;
static struct konsole* s_hl_ks          = NULL;
static uint32_t        s_hl_last_report = 0;
static uint32_t        s_hl_last_total  = 0;  /* for delta reporting */
static int             s_hl_last_log    = 0;  /* deauth log watermark */

/* Stop whatever is currently active */
static void hl_stop(void) {
    switch (s_hl_mode) {
#ifdef BS_HAS_WIFI
        case HL_WIFI_BEACON:
            beacon_svc_stop();
            bs_wifi_deinit();
            break;
        case HL_WIFI_DEAUTH:
            deauth_svc_stop();
            bs_wifi_deinit();
            break;
        case HL_WIFI_SNIFF:
            sniffer_svc_stop();
            bs_wifi_deinit();
            break;
#endif
#ifdef BS_HAS_BLE
        case HL_BLE_SPAM:
            ble_spam_svc_stop();
            bs_ble_deinit();
            break;
        case HL_BLE_SCAN:
            ble_scan_svc_stop();
            bs_ble_deinit();
            break;
#endif
        default: break;
    }
    s_hl_mode        = HL_IDLE;
    s_hl_ks          = NULL;
    s_hl_last_report = 0;
    s_hl_last_total  = 0;
    s_hl_last_log    = 0;
}

/* Ticked every loop iteration (both headless and menu idle) */
static void hl_tick(void) {
    if (s_hl_mode == HL_IDLE || !s_hl_ks) return;
    uint32_t now = s_arch->millis();

#ifdef BS_HAS_WIFI
    if (s_hl_mode == HL_WIFI_BEACON) {
        beacon_svc_tick(now);
        if (now - s_hl_last_report >= 5000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[beacon] %lu frames  pps=%lu  ssid=%s  ch=%u\r\n",
                       (unsigned long)beacon_svc_frames(),
                       (unsigned long)beacon_svc_pps(),
                       beacon_svc_cur_ssid(),
                       (unsigned)beacon_svc_cur_channel());
        }
        return;
    }

    if (s_hl_mode == HL_WIFI_DEAUTH) {
        deauth_svc_tick(now);
        /* Print new service log entries as they arrive */
        if (deauth_svc_log_dirty()) {
            int n = deauth_svc_log_count();
            for (int i = s_hl_last_log; i < n; i++)
                kon_printf(s_hl_ks, "[deauth] %s\r\n", deauth_svc_log_line(i));
            s_hl_last_log = n;
            deauth_svc_log_clear_dirty();
        }
        if (now - s_hl_last_report >= 5000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[deauth] %lu frames  pps=%lu\r\n",
                       (unsigned long)deauth_svc_frames(),
                       (unsigned long)deauth_svc_pps());
        }
        return;
    }

    if (s_hl_mode == HL_WIFI_SNIFF) {
        sniffer_svc_tick(now);
        if (now - s_hl_last_report >= 3000) {
            s_hl_last_report = now;
            uint32_t tot   = sniffer_svc_count();
            uint32_t delta = tot - s_hl_last_total;
            s_hl_last_total = tot;
            kon_printf(s_hl_ks, "[sniff] total=%lu  +%lu/3s  ch=%u  drop=%lu\r\n",
                       (unsigned long)tot, (unsigned long)delta,
                       (unsigned)sniffer_svc_channel(),
                       (unsigned long)sniffer_svc_dropped());
        }
        return;
    }
#endif /* BS_HAS_WIFI */

#ifdef BS_HAS_BLE
    if (s_hl_mode == HL_BLE_SPAM) {
        ble_spam_svc_tick(now);
        if (now - s_hl_last_report >= 5000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[ble-spam] %lu adverts  mode=%s  ivl=%lums\r\n",
                       (unsigned long)ble_spam_svc_count(),
                       k_ble_spam_mode_names[ble_spam_svc_mode()],
                       (unsigned long)ble_spam_svc_interval_ms());
        }
        return;
    }

    if (s_hl_mode == HL_BLE_SCAN) {
        /* BLE scan is callback-driven; no tick needed — just report */
        if (now - s_hl_last_report >= 3000) {
            s_hl_last_report = now;
            int  tot   = ble_scan_svc_count();
            uint32_t delta = (uint32_t)tot - s_hl_last_total;
            s_hl_last_total = (uint32_t)tot;
            kon_printf(s_hl_ks, "[ble-scan] total=%d  +%lu/3s\r\n",
                       tot, (unsigned long)delta);
        }
        return;
    }
#endif /* BS_HAS_BLE */
}

/* ---- 'wifi' command ---------------------------------------------------- */
#ifdef BS_HAS_WIFI
static int cmd_wifi(struct konsole* ks, int argc, char** argv) {
    if (argc < 2) {
        uint32_t caps = bs_wifi_caps();
        char capbuf[64] = "";
        if (caps & BS_WIFI_CAP_SCAN)    { strcat(capbuf, "SCAN ");    }
        if (caps & BS_WIFI_CAP_CONNECT) { strcat(capbuf, "CONNECT "); }
        if (caps & BS_WIFI_CAP_SNIFF)   { strcat(capbuf, "SNIFF ");   }
        if (caps & BS_WIFI_CAP_MONITOR) { strcat(capbuf, "MONITOR "); }
        if (caps & BS_WIFI_CAP_INJECT)  { strcat(capbuf, "INJECT ");  }
        /* trim trailing space */
        int clen = (int)strlen(capbuf);
        if (clen > 0 && capbuf[clen-1] == ' ') capbuf[clen-1] = '\0';
        kon_printf(ks, "WiFi caps: %s\r\n", capbuf[0] ? capbuf : "(none)");
        switch (s_hl_mode) {
            case HL_WIFI_BEACON:
                kon_printf(ks, "Active: beacon spam  %lu frames  pps=%lu\r\n",
                           (unsigned long)beacon_svc_frames(),
                           (unsigned long)beacon_svc_pps()); break;
            case HL_WIFI_DEAUTH:
                kon_printf(ks, "Active: deauth flood  %lu frames  pps=%lu\r\n",
                           (unsigned long)deauth_svc_frames(),
                           (unsigned long)deauth_svc_pps()); break;
            case HL_WIFI_SNIFF:
                kon_printf(ks, "Active: sniffer  %lu packets  ch=%u\r\n",
                           (unsigned long)sniffer_svc_count(),
                           (unsigned)sniffer_svc_channel()); break;
            default:
                kon_printf(ks, "Idle.\r\n"); break;
        }
        return 0;
    }

    const char* sub  = argv[1];
    const char* verb = (argc >= 3) ? argv[2] : "";

    /* ---- beacon ---- */
    if (bs_stricmp(sub, "beacon") == 0) {
        if (bs_stricmp(verb, "stop") == 0) {
            if (s_hl_mode == HL_WIFI_BEACON) { hl_stop(); kon_printf(ks, "Beacon spam stopped.\r\n"); }
            else kon_printf(ks, "Not running.\r\n");
            return 0;
        }
        if (bs_stricmp(verb, "start") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
            /* Parse options */
            beacon_svc_mode_t  mode    = BEACON_MODE_RANDOM;
            wifi_charset_t     charset = WIFI_CHARSET_ASCII;
            char               prefix[21] = "testAP";
            int                repeat  = 3;
            uint8_t            channel = 6;
            for (int i = 3; i < argc - 1; i++) {
                if (strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
                    if (strcmp(argv[i+1], "custom") == 0) mode = BEACON_MODE_CUSTOM;
                    else if (strcmp(argv[i+1], "file") == 0) mode = BEACON_MODE_FILE;
                    i++;
                } else if (strcmp(argv[i], "--prefix") == 0 && i+1 < argc) {
                    strncpy(prefix, argv[i+1], sizeof prefix - 1);
                    mode = BEACON_MODE_CUSTOM; i++;
                } else if (strcmp(argv[i], "--charset") == 0 && i+1 < argc) {
                    if      (strcmp(argv[i+1],"hi")   == 0) charset = WIFI_CHARSET_HIRAGANA;
                    else if (strcmp(argv[i+1],"kata") == 0) charset = WIFI_CHARSET_KATAKANA;
                    else if (strcmp(argv[i+1],"cyr")  == 0) charset = WIFI_CHARSET_CYRILLIC;
                    i++;
                } else if (strcmp(argv[i], "--ch") == 0 && i+1 < argc) {
                    int ch = atoi(argv[i+1]);
                    if (ch >= 1 && ch <= 13) channel = (uint8_t)ch;
                    i++;
                } else if (strcmp(argv[i], "--repeat") == 0 && i+1 < argc) {
                    repeat = atoi(argv[i+1]); i++;
                }
            }
            beacon_svc_init(s_arch);
            beacon_svc_start(mode, charset, prefix, repeat, NULL, 0);
            /* Seed the channel (service picks random; override if user specified) */
            (void)channel;  /* service picks per-beacon random channel */
            s_hl_mode = HL_WIFI_BEACON;
            s_hl_ks   = ks;
            s_hl_last_report = 0;
            kon_printf(ks, "Beacon spam started (mode=%s).\r\n"
                           "Reports every 5 s.  'wifi beacon stop' to halt.\r\n",
                       mode == BEACON_MODE_CUSTOM ? "custom" :
                       mode == BEACON_MODE_FILE   ? "file"   : "random");
            return 0;
        }
        kon_printf(ks, "Usage: wifi beacon start|stop [--mode random|custom|file] "
                       "[--prefix P] [--charset ascii|hi|kata|cyr] [--repeat N]\r\n");
        return 0;
    }

    /* ---- deauth ---- */
    if (bs_stricmp(sub, "deauth") == 0) {
        if (bs_stricmp(verb, "stop") == 0) {
            if (s_hl_mode == HL_WIFI_DEAUTH) { hl_stop(); kon_printf(ks, "Deauth stopped.\r\n"); }
            else kon_printf(ks, "Not running.\r\n");
            return 0;
        }
        if (bs_stricmp(verb, "start") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
            uint8_t channel = 1;
            for (int i = 3; i < argc - 1; i++) {
                if (strcmp(argv[i], "--ch") == 0 && i+1 < argc) {
                    int ch = atoi(argv[i+1]);
                    if (ch >= 1 && ch <= 13) channel = (uint8_t)ch;
                    i++;
                }
            }
            deauth_svc_init(s_arch);
            deauth_svc_attack_broadcast(channel);  /* shortcut: no scan needed */
            s_hl_mode        = HL_WIFI_DEAUTH;
            s_hl_ks          = ks;
            s_hl_last_report = 0;
            s_hl_last_log    = 0;
            kon_printf(ks, "Deauth flood started (ch=%u, broadcast).\r\n"
                           "Reports every 5 s.  'wifi deauth stop' to halt.\r\n",
                       channel);
            return 0;
        }
        kon_printf(ks, "Usage: wifi deauth start|stop [--ch N]\r\n");
        return 0;
    }

    /* ---- sniff ---- */
    if (bs_stricmp(sub, "sniff") == 0) {
        if (bs_stricmp(verb, "stop") == 0) {
            if (s_hl_mode == HL_WIFI_SNIFF) { hl_stop(); kon_printf(ks, "Sniffer stopped.\r\n"); }
            else kon_printf(ks, "Not running.\r\n");
            return 0;
        }
        if (bs_stricmp(verb, "start") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
            uint8_t channel  = 0;  /* 0 = auto-hop */
            for (int i = 3; i < argc - 1; i++) {
                if (strcmp(argv[i], "--ch") == 0 && i+1 < argc) {
                    int ch = atoi(argv[i+1]);
                    if (ch >= 1 && ch <= 13) channel = (uint8_t)ch;
                    i++;
                }
            }
            sniffer_svc_init(s_arch);
            sniffer_svc_start(channel, 500, NULL, NULL);  /* no pkt callback in CLI */
            s_hl_mode        = HL_WIFI_SNIFF;
            s_hl_ks          = ks;
            s_hl_last_report = 0;
            s_hl_last_total  = 0;
            kon_printf(ks, "Sniffer started (ch=%u, %s).\r\n"
                           "Reports every 3 s.  'wifi sniff stop' to halt.\r\n",
                       channel ? channel : 1,
                       channel ? "fixed" : "auto-hop");
            return 0;
        }
        kon_printf(ks, "Usage: wifi sniff start|stop [--ch N]\r\n");
        return 0;
    }

    kon_printf(ks, "Usage: wifi beacon|deauth|sniff start|stop [...]\r\n");
    return 0;
}
#endif /* BS_HAS_WIFI */

/* ---- 'ble' command ----------------------------------------------------- */
#ifdef BS_HAS_BLE
static int cmd_ble(struct konsole* ks, int argc, char** argv) {
    if (argc < 2) {
        uint32_t caps = bs_ble_caps();
        kon_printf(ks, "BLE caps: %s%s%s\r\n",
                   (caps & BS_BLE_CAP_ADVERTISE) ? "ADVERTISE " : "",
                   (caps & BS_BLE_CAP_SCAN)      ? "SCAN "      : "",
                   (caps & BS_BLE_CAP_RAND_ADDR) ? "RAND_ADDR"  : "");
        switch (s_hl_mode) {
            case HL_BLE_SPAM:
                kon_printf(ks, "Active: BLE spam  %lu adverts  mode=%s  ivl=%lums\r\n",
                           (unsigned long)ble_spam_svc_count(),
                           k_ble_spam_mode_names[ble_spam_svc_mode()],
                           (unsigned long)ble_spam_svc_interval_ms()); break;
            case HL_BLE_SCAN:
                kon_printf(ks, "Active: BLE scan  %d devices\r\n",
                           ble_scan_svc_count()); break;
            default:
                kon_printf(ks, "Idle.\r\n"); break;
        }
        return 0;
    }

    const char* sub  = argv[1];
    const char* verb = (argc >= 3) ? argv[2] : "";

    /* ---- spam ---- */
    if (bs_stricmp(sub, "spam") == 0) {
        if (bs_stricmp(verb, "stop") == 0) {
            if (s_hl_mode == HL_BLE_SPAM) { hl_stop(); kon_printf(ks, "BLE spam stopped.\r\n"); }
            else kon_printf(ks, "Not running.\r\n");
            return 0;
        }
        if (bs_stricmp(verb, "start") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_ble_init(s_arch) < 0) { kon_printf(ks, "BLE init failed.\r\n"); return 1; }
            ble_spam_mode_t mode       = BLE_SPAM_ALL;
            uint32_t        interval   = 100;
            for (int i = 3; i < argc - 1; i++) {
                if (strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
                    const char* m = argv[i+1];
                    if      (bs_stricmp(m,"apple")   == 0) mode = BLE_SPAM_APPLE;
                    else if (bs_stricmp(m,"samsung") == 0) mode = BLE_SPAM_SAMSUNG;
                    else if (bs_stricmp(m,"google")  == 0) mode = BLE_SPAM_GOOGLE;
                    else if (bs_stricmp(m,"name")    == 0) mode = BLE_SPAM_NAME;
                    else if (bs_stricmp(m,"all")     == 0) mode = BLE_SPAM_ALL;
                    i++;
                } else if (strcmp(argv[i], "--ivl") == 0 && i+1 < argc) {
                    int ms = atoi(argv[i+1]);
                    if (ms > 0) interval = (uint32_t)ms;
                    i++;
                }
            }
            ble_spam_svc_init(s_arch);
            ble_spam_svc_start(mode, interval);
            s_hl_mode        = HL_BLE_SPAM;
            s_hl_ks          = ks;
            s_hl_last_report = 0;
            kon_printf(ks, "BLE spam started (mode=%s  ivl=%lums).\r\n"
                           "Reports every 5 s.  'ble spam stop' to halt.\r\n",
                       k_ble_spam_mode_names[mode], (unsigned long)interval);
            return 0;
        }
        kon_printf(ks, "Usage: ble spam start|stop [--mode apple|samsung|google|name|all] [--ivl ms]\r\n");
        return 0;
    }

    /* ---- scan ---- */
    if (bs_stricmp(sub, "scan") == 0) {
        if (bs_stricmp(verb, "stop") == 0) {
            if (s_hl_mode == HL_BLE_SCAN) { hl_stop(); kon_printf(ks, "BLE scan stopped.\r\n"); }
            else kon_printf(ks, "Not running.\r\n");
            return 0;
        }
        if (bs_stricmp(verb, "start") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_ble_init(s_arch) < 0) { kon_printf(ks, "BLE init failed.\r\n"); return 1; }
            ble_scan_svc_init(s_arch);
            ble_scan_svc_start();
            s_hl_mode        = HL_BLE_SCAN;
            s_hl_ks          = ks;
            s_hl_last_report = 0;
            s_hl_last_total  = 0;
            kon_printf(ks, "BLE scan started.\r\n"
                           "Reports every 3 s.  'ble scan stop' to halt.\r\n");
            return 0;
        }
        kon_printf(ks, "Usage: ble scan start|stop\r\n");
        return 0;
    }

    kon_printf(ks, "Usage: ble spam|scan start|stop [...]\r\n");
    return 0;
}
#endif /* BS_HAS_BLE */

/* ---- 'opts' command ---------------------------------------------------- */
/*
 * opts                     - list all current settings
 * opts <key> <value>       - set a setting
 * opts save                - persist settings to FS
 * opts sdinfo              - show SD card status
 * opts sdformat [yes]      - format SD card (replaces top-level sdformat)
 *
 * Keys: text_scale  brightness  layout  palette  border  voltage
 */

/* Palette / border names come from bs_theme — declare enough for lookup */
static int cmd_opts(struct konsole* ks, int argc, char** argv) {
    /* opts sdinfo */
    if (argc >= 2 && bs_stricmp(argv[1], "sdinfo") == 0) {
        if (bs_fs_available()) {
            long log_sz = bs_fs_file_size("system.log");
            if (log_sz >= 0)
                kon_printf(ks, "SD: mounted  log=%ld B\r\n", log_sz);
            else
                kon_printf(ks, "SD: mounted\r\n");
        } else {
#ifdef BS_FS_SDCARD
            const char* e = bs_fs_init_error();
            kon_printf(ks, "SD: not mounted  (%s)\r\n", e ? e : "unknown");
#else
            kon_printf(ks, "SD: not configured\r\n");
#endif
        }
        return 0;
    }

    /* opts sdformat [yes] */
    if (argc >= 2 && bs_stricmp(argv[1], "sdformat") == 0) {
        if (argc < 3 || strcmp(argv[2], "yes") != 0) {
            kon_printf(ks, "WARNING: erases ALL data on the SD card.\r\n");
            kon_printf(ks, "Run 'opts sdformat yes' to confirm.\r\n");
            return 0;
        }
        kon_printf(ks, "Formatting SD card as FAT32, please wait...\r\n");
        if (bs_fs_format() == 0) {
            bs_log_flush_sd();
            kon_printf(ks, "Done - SD formatted and mounted.\r\n");
        } else {
            const char* e = bs_fs_init_error();
            kon_printf(ks, "Failed: %s\r\n", e ? e : "unknown error");
        }
        return 0;
    }

    /* opts save */
    if (argc >= 2 && bs_stricmp(argv[1], "save") == 0) {
        if (!bs_fs_available()) {
            kon_printf(ks, "No storage available — settings not saved.\r\n");
            return 1;
        }
        char buf[256];
        float ts = bs_ui_text_scale();
        int n = snprintf(buf, sizeof buf,
            "layout=%d\ntext_scale=%.1f\nbrightness=%d\nshow_voltage=%d\n"
            "grid_cols=%d\ngrid_rows=%d\n",
            (int)bs_menu_get_mode(), (double)ts, bs_ui_brightness(),
            (int)bs_ui_show_voltage(),
            bs_ui_grid_max_cols(), bs_ui_grid_max_rows());
        bs_fs_write_file("settings.cfg", buf, (size_t)n);
        kon_printf(ks, "Settings saved.\r\n");
        return 0;
    }

    /* opts <key> <value>  — set a setting live */
    if (argc >= 3) {
        const char* key = argv[1];
        const char* val = argv[2];

        if (bs_stricmp(key, "text_scale") == 0) {
            float f = (float)atof(val);
            if (f < 1.0f || f > 3.0f) {
                /* Accept names */
                if      (bs_stricmp(val,"small")  == 0) f = 1.0f;
                else if (bs_stricmp(val,"mid-s")  == 0) f = 1.5f;
                else if (bs_stricmp(val,"medium") == 0) f = 2.0f;
                else if (bs_stricmp(val,"mid-l")  == 0) f = 2.5f;
                else if (bs_stricmp(val,"large")  == 0) f = 3.0f;
                else { kon_printf(ks, "text_scale: small|mid-s|medium|mid-l|large or 1.0-3.0\r\n"); return 1; }
            }
            bs_ui_set_text_scale(f);
            kon_printf(ks, "text_scale=%.1f\r\n", (double)bs_ui_text_scale());
            return 0;
        }
        if (bs_stricmp(key, "brightness") == 0) {
            int pct = atoi(val);
            bs_ui_set_brightness(pct);
            kon_printf(ks, "brightness=%d%%\r\n", bs_ui_brightness());
            return 0;
        }
        if (bs_stricmp(key, "layout") == 0) {
            int idx = -1;
            if      (bs_stricmp(val,"auto")      == 0) idx = 0;
            else if (bs_stricmp(val,"grid")      == 0) idx = 1;
            else if (bs_stricmp(val,"slideshow") == 0) idx = 2;
            else if (bs_stricmp(val,"list")      == 0) idx = 3;
            else idx = atoi(val);
            if (idx < 0 || idx > 3) { kon_printf(ks, "layout: auto|grid|slideshow|list or 0-3\r\n"); return 1; }
            bs_menu_set_mode((bs_menu_mode_t)idx);
            kon_printf(ks, "layout=%d\r\n", idx);
            return 0;
        }
        if (bs_stricmp(key, "voltage") == 0) {
            bool on = (bs_stricmp(val,"on")==0 || strcmp(val,"1")==0);
            bs_ui_set_show_voltage(on);
            kon_printf(ks, "voltage=%s\r\n", on ? "on" : "off");
            return 0;
        }
        if (bs_stricmp(key, "grid_cols") == 0) {
            bs_ui_set_grid_max_cols(atoi(val));
            kon_printf(ks, "grid_cols=%d\r\n", bs_ui_grid_max_cols());
            return 0;
        }
        if (bs_stricmp(key, "grid_rows") == 0) {
            bs_ui_set_grid_max_rows(atoi(val));
            kon_printf(ks, "grid_rows=%d\r\n", bs_ui_grid_max_rows());
            return 0;
        }
        kon_printf(ks, "Unknown key '%s'.\r\n", key);
        kon_printf(ks, "Keys: text_scale brightness layout voltage grid_cols grid_rows\r\n");
        return 1;
    }

    /* opts — list all current settings */
    static const char* const k_scale_names[]  = {"Small","Mid-S","Medium","Mid-L","Large"};
    static const float        k_scale_vals[]   = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
    static const char* const k_layout_names[] = {"Auto","Grid","Slideshow","List"};
    float ts  = bs_ui_text_scale();
    int   lm  = (int)bs_menu_get_mode();
    const char* ts_name = "?";
    for (int i = 0; i < 5; i++)
        if (k_scale_vals[i] == ts) { ts_name = k_scale_names[i]; break; }

    kon_printf(ks, "Settings:\r\n");
    kon_printf(ks, "  text_scale  = %.1f (%s)\r\n", (double)ts, ts_name);
    kon_printf(ks, "  brightness  = %d%%\r\n", bs_ui_brightness());
    kon_printf(ks, "  layout      = %d (%s)\r\n", lm,
               lm >= 0 && lm <= 3 ? k_layout_names[lm] : "?");
    kon_printf(ks, "  voltage     = %s\r\n", bs_ui_show_voltage() ? "on" : "off");
    kon_printf(ks, "  grid_cols   = %d\r\n", bs_ui_grid_max_cols());
    kon_printf(ks, "  grid_rows   = %d\r\n", bs_ui_grid_max_rows());
    kon_printf(ks, "  firmware    = " BS_FW_NAME " v" BS_VERSION "\r\n");
    if (bs_fs_available())
        kon_printf(ks, "  sd          = mounted\r\n");
    else
        kon_printf(ks, "  sd          = not mounted\r\n");
    kon_printf(ks, "Use 'opts <key> <value>' to change.  'opts save' to persist.\r\n");
    kon_printf(ks, "Use 'opts sdinfo' for SD details, 'opts sdformat yes' to erase+format.\r\n");
    return 0;
}

static const struct kon_cmd k_cmds[] = {
    { "help",         "show commands",              cmd_help         },
    { "hw",           "hardware & system status",   cmd_hw           },
    { "dmesg",        "show log (dmesg flush->SD)", cmd_dmesg        },
    { "sdformat",     "format SD card as FAT32",    cmd_sdformat     },
    { "opts",         "show/set settings (opts sdinfo|sdformat)",  cmd_opts },
    { "startapp",     "launch app by name",         cmd_startapp     },
#ifdef BS_HAS_WIFI
    { "wifi",  "wifi beacon|deauth|sniff start|stop [options]", cmd_wifi  },
#endif
#ifdef BS_HAS_BLE
    { "ble",   "ble spam|scan start|stop [options]",            cmd_ble   },
#endif
};

/* ---- bs_init ---------------------------------------------------------- */
void bs_init(void) {
    s_arch = arch_bs();
    s_arch->init();
    s_arch->uart_init(0, BS_UART_BAUD_VAL);

    /* Graphics backend */
    bs_gfx_init(s_arch);

    /* Input backend */
    bs_keys_init(s_arch);

    /* Konsole (serial CLI) */
    struct konsole_io io = {
        .read_avail = io_read_avail,
        .read       = io_read,
        .write      = io_write,
        .millis     = io_millis,
        .ctx        = NULL,
    };
    konsole_init_with_storage(&g_ks, &g_ks_line, &io,
                              k_cmds, sizeof k_cmds / sizeof k_cmds[0],
                              BS_FW_NAME "> ", /*vt100*/ true);

    /* Logging layer */
    bs_log_init(&g_ks);
#ifdef ARCH_ESP32
    bs_log_esp_hook_init();   /* redirect ESP-IDF/Arduino log_e/log_w → bs_log ring */
#endif

    /* Debug overlay */
    bs_debug_init(s_arch);

#ifdef BS_USE_SIC
    /* SIC must init before fs: XL9555 GPIO14 (SD power enable) is set by preinit */
    sic_i2c_begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
    sic_begin_legacy(&BS_SIC_BOARD, NULL);
#endif

    /* Filesystem - SD power is now on (XL9555 GPIO14+GPIO13 set by SIC preinit) */
    bs_fs_init();
    bs_log_flush_sd();   /* write boot log ring to system.log on SD */

    /* Log PSRAM and free heap after all static init is done.               */
#if defined(ARCH_ESP32)
    {
        bs_hw_info_t hinfo;
        bs_hw_get_info(&hinfo);
        if (hinfo.psram_total_kb > 0) {
            BS_LOGOK("psram", "%u kB total  %u kB free",
                     hinfo.psram_total_kb, hinfo.psram_free_kb);
        } else {
            BS_LOGBW("psram", "not found  internal heap=%u kB free",
                     hinfo.heap_free_kb);
        }
    }
#endif

    /* Theme + UI */
    bs_theme_set(&bs_theme_orange);

#ifndef BS_HEADLESS
    bs_menu_init(k_apps, k_n_apps, BS_MENU_AUTO, menu_idle_poll);
    bs_ui_load_settings();   /* applies saved theme, layout, text_scale, brightness */
    bs_boot_run(s_arch, boot_idle_poll);
#else
    kon_banner(&g_ks, NULL);
#endif
}

/* ---- bs_run ----------------------------------------------------------- */
void bs_run(void) {
#ifdef BS_HEADLESS
    konsole_poll(&g_ks);
    hl_tick();
    s_arch->delay_ms(1);
#else
    const bs_app_t* app = bs_menu_run(s_arch);
    if (app && app->run) {
        bs_gfx_clear(g_bs_theme.bg);
        app->run(s_arch);
        bs_gfx_clear(g_bs_theme.bg);
        bs_menu_invalidate();
    }
#endif
}
