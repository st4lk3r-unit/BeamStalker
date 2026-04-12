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
#include "bs/bs_board.h"
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
#  include "apps/wifi/wifi_eapol_svc.h"
#endif
#if defined(BS_HAS_WIFI) && defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
#  include "apps/wifi/wifi_captive_svc.h"
#  include "apps/wifi/wifi_eviltwin_svc.h"
#  include "apps/wifi/wifi_honeypot_svc.h"
#  include "apps/wifi/wifi_karma_svc.h"
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


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
        long log_sz = bs_fs_file_size(BS_PATH_LOG);
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

    if (bs_board_caps() & BS_BOARD_CAP_XL9555_DIAG) {
        /* XL9555 GPIO expander diagnostic */
        bs_board_diag_t xl;
        bs_board_diag_read(&xl);
        if (!xl.available) {
            kon_printf(ks, "  XL9555: not configured\r\n");
        } else if (!xl.reachable) {
            kon_printf(ks, "  XL9555: [ ERR ] I2C 0x20 not responding\r\n");
        } else {
            bool sd_det = !(xl.input_1  & (1 << 2));
            bool sd_en  =  (xl.output_1 & (1 << 4));
            bool sd_out = !(xl.config_1 & (1 << 4));
            kon_printf(ks, "  XL9555: INPUT_1=0x%02X  CFG_1=0x%02X  OUT_1=0x%02X\r\n",
                       xl.input_1, xl.config_1, xl.output_1);
            kon_printf(ks, "          SD_DET(P12)=%s  SD_EN(P14)=%s(%s)\r\n",
                       sd_det ? "card" : "NO CARD",
                       sd_out ? (sd_en ? "HIGH" : "LOW") : "INPUT!",
                       sd_out ? "out" : "BUG:in");
        }
    }

    kon_printf(ks, "  Up    : %.1f s\r\n", (float)s_arch->millis() / 1000.0f);
    return 0;
}

static int cmd_dmesg(struct konsole* ks, int argc, char** argv) {
    /* dmesg         - print in-memory log (boot to now)
     * dmesg flush   - also write current ring to SD BeamStalker/system.log */
    bool do_flush = (argc >= 2 && strcmp(argv[1], "flush") == 0);

    bs_log_print_all(ks);

    if (do_flush) {
        bs_log_flush_sd();
        if (bs_fs_available())
            kon_printf(ks, "--- flushed to BeamStalker/system.log ---\r\n");
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
 * Commands (all block until q / Ctrl+C; use --help for full options):
 *   wifi                          show WiFi/attack status
 *   wifi scan    [--timeout N]    passive AP discovery (beacon+probe-resp sniff)
 *   wifi beacon  [OPTIONS]        beacon spam, rotating channels
 *   wifi deauth  [OPTIONS]        deauth flood — broadcast, targeted, or per-client
 *   wifi deauth  scan             active AP scan (prints table, no attack)
 *   wifi sniff   [OPTIONS]        passive capture, filters, pcap, --timeout N
 *   wifi captive [OPTIONS]        rogue AP + captive portal      (ESP32 only)
 *   wifi eapol   [OPTIONS]        WPA2 handshake capture + optional deauth trigger
 *   wifi eviltwin [OPTIONS]       clone AP + deauth-triggered redirect chain (ESP32 only)
 *   wifi honeypot [OPTIONS]       clone AP + CSA/deauth lures    (ESP32 only)
 *   wifi karma   [OPTIONS]        probe-response rogue AP        (ESP32 only)
 *   ble spam     [OPTIONS]        BLE advertisement flood
 *   ble scan                      passive BLE device discovery
 */

typedef enum {
    HL_IDLE = 0,
    HL_WIFI_BEACON,
    HL_WIFI_DEAUTH,
    HL_WIFI_SNIFF,
    HL_WIFI_EAPOL,
    HL_WIFI_CAPTIVE,    /* ESP32+Arduino only — portal-based features below */
    HL_WIFI_EVILTWIN,
    HL_WIFI_HONEYPOT,
    HL_WIFI_KARMA,
    HL_BLE_SPAM,
    HL_BLE_SCAN,
} hl_mode_t;

static hl_mode_t       s_hl_mode        = HL_IDLE;
static struct konsole* s_hl_ks          = NULL;
static uint32_t        s_hl_last_report = 0;
static uint32_t        s_hl_last_total  = 0;  /* for delta reporting */
static int             s_hl_last_log    = 0;  /* deauth log watermark */
static int             s_hl_last_cred   = 0;  /* portal cred watermark */
static int             s_hl_last_clients= 0;  /* portal client watermark */
static int             s_hl_last_ssids  = 0;  /* karma SSID watermark */
static uint32_t        s_hl_timeout_ms  = 0;  /* 0 = no timeout */

/* Sniff session context — declared here so hl_stop/hl_tick can reference it */
#ifdef BS_HAS_WIFI
#define SNIFF_FTYPE_ALL  0xFF
typedef struct {
    struct konsole* ks;
    bool            verbose;
    uint8_t         filt_type;
    uint8_t         filt_subtype;
    bool            has_src;   uint8_t filt_src[6];
    bool            has_dst;   uint8_t filt_dst[6];
    bool            has_bssid; uint8_t filt_bssid[6];
    bs_file_t       pcap_f;
    uint32_t        pcap_count;
} sniff_ctx_t;
static sniff_ctx_t s_sniff_ctx;
#endif /* BS_HAS_WIFI */

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
            if (s_sniff_ctx.pcap_f) {
                bs_fs_close(s_sniff_ctx.pcap_f);
                kon_printf(s_hl_ks ? s_hl_ks : &g_ks,
                           "[sniff] pcap closed  %lu frames\r\n",
                           (unsigned long)s_sniff_ctx.pcap_count);
                s_sniff_ctx.pcap_f = NULL;
            }
            break;
        case HL_WIFI_EAPOL: {
            uint32_t pcap_n = eapol_svc_pcap_frames();
            eapol_svc_stop();
            bs_wifi_deinit();
            if (pcap_n > 0)
                kon_printf(s_hl_ks ? s_hl_ks : &g_ks,
                           "[eapol] pcap closed  %lu frames\r\n",
                           (unsigned long)pcap_n);
            break;
        }
#  if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
        case HL_WIFI_CAPTIVE:
            captive_svc_stop();
            bs_wifi_deinit();
            break;
        case HL_WIFI_EVILTWIN:
            eviltwin_svc_stop();
            bs_wifi_deinit();
            break;
        case HL_WIFI_HONEYPOT:
            honeypot_svc_stop();
            bs_wifi_deinit();
            break;
        case HL_WIFI_KARMA:
            karma_svc_stop();
            bs_wifi_deinit();
            break;
#  endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
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
    s_hl_mode         = HL_IDLE;
    s_hl_ks           = NULL;
    s_hl_last_report  = 0;
    s_hl_last_total   = 0;
    s_hl_last_log     = 0;
    s_hl_last_cred    = 0;
    s_hl_last_clients = 0;
    s_hl_last_ssids   = 0;
    s_hl_timeout_ms   = 0;
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
        /* In verbose mode the pkt callback prints every frame; just show rate summary */
        if (now - s_hl_last_report >= (s_sniff_ctx.verbose ? 10000u : 3000u)) {
            s_hl_last_report = now;
            uint32_t tot   = sniffer_svc_count();
            uint32_t delta = tot - s_hl_last_total;
            s_hl_last_total = tot;
            kon_printf(s_hl_ks, "[sniff] total=%lu  +%lu  ch=%u  drop=%lu%s\r\n",
                       (unsigned long)tot, (unsigned long)delta,
                       (unsigned)sniffer_svc_channel(),
                       (unsigned long)sniffer_svc_dropped(),
                       s_sniff_ctx.pcap_f ? "  [pcap]" : "");
        }
        return;
    }

    if (s_hl_mode == HL_WIFI_EAPOL) {
        eapol_svc_tick(now);
        if (now - s_hl_last_report >= 3000) {
            s_hl_last_report = now;
            int  pairs = eapol_svc_pair_count();
            if (pairs > s_hl_last_clients) {
                /* New complete M1+M2 pair captured */
                kon_printf(s_hl_ks,
                    "[eapol] handshake captured!  pairs=%d  eapol=%lu  ch=%u\r\n",
                    pairs, (unsigned long)eapol_svc_eapol_count(),
                    (unsigned)sniffer_svc_channel());
                s_hl_last_clients = pairs;
            } else {
                kon_printf(s_hl_ks,
                    "[eapol] eapol=%lu  pairs=%d  ch=%u\r\n",
                    (unsigned long)eapol_svc_eapol_count(),
                    pairs, (unsigned)sniffer_svc_channel());
            }
        }
        return;
    }

#  if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
    /* ---- captive portal ---- */
    if (s_hl_mode == HL_WIFI_CAPTIVE) {
        captive_svc_tick(now);
        int nl = captive_svc_client_count();
        if (nl != s_hl_last_clients) {
            kon_printf(s_hl_ks, nl > s_hl_last_clients
                ? "[captive] client connected     total=%d\r\n"
                : "[captive] client disconnected  total=%d\r\n", nl);
            s_hl_last_clients = nl;
        }
        int nc = captive_svc_cred_count();
        if (nc > s_hl_last_cred) {
            for (int i = s_hl_last_cred; i < nc; i++) {
                const wifi_portal_cred_t* c = captive_svc_get_cred(i);
                if (c) kon_printf(s_hl_ks, "[captive] +cred #%d  user=\"%.32s\"  pass=\"%.32s\"\r\n",
                                  i + 1, c->user, c->pass);
            }
            s_hl_last_cred = nc;
        }
        if (now - s_hl_last_report >= 30000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[captive] running  clients=%d  creds=%d\r\n", nl, nc);
        }
        return;
    }

    /* ---- evil twin ---- */
    if (s_hl_mode == HL_WIFI_EVILTWIN) {
        eviltwin_svc_tick(now);
        int nl = eviltwin_svc_client_count();
        if (nl != s_hl_last_clients) {
            kon_printf(s_hl_ks, nl > s_hl_last_clients
                ? "[eviltwin] client connected     total=%d\r\n"
                : "[eviltwin] client disconnected  total=%d\r\n", nl);
            s_hl_last_clients = nl;
        }
        int nc = eviltwin_svc_cred_count();
        if (nc > s_hl_last_cred) {
            for (int i = s_hl_last_cred; i < nc; i++) {
                const wifi_portal_cred_t* c = eviltwin_svc_get_cred(i);
                if (c) kon_printf(s_hl_ks, "[eviltwin] +cred #%d  user=\"%.32s\"  pass=\"%.32s\"\r\n",
                                  i + 1, c->user, c->pass);
            }
            s_hl_last_cred = nc;
        }
        if (now - s_hl_last_report >= 30000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[eviltwin] running  clients=%d  creds=%d  deauth=%lu\r\n",
                       nl, nc, (unsigned long)eviltwin_svc_deauth_total());
        }
        return;
    }

    /* ---- honeypot ---- */
    if (s_hl_mode == HL_WIFI_HONEYPOT) {
        honeypot_svc_tick(now);
        int nl = honeypot_svc_client_count();
        if (nl != s_hl_last_clients) {
            kon_printf(s_hl_ks, nl > s_hl_last_clients
                ? "[honeypot] client connected     total=%d\r\n"
                : "[honeypot] client disconnected  total=%d\r\n", nl);
            s_hl_last_clients = nl;
        }
        int nc = honeypot_svc_cred_count();
        if (nc > s_hl_last_cred) {
            for (int i = s_hl_last_cred; i < nc; i++) {
                const wifi_portal_cred_t* c = honeypot_svc_get_cred(i);
                if (c) kon_printf(s_hl_ks, "[honeypot] +cred #%d  user=\"%.32s\"  pass=\"%.32s\"\r\n",
                                  i + 1, c->user, c->pass);
            }
            s_hl_last_cred = nc;
        }
        if (now - s_hl_last_report >= 30000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[honeypot] running  clients=%d  creds=%d  lures=%lu\r\n",
                       nl, nc, (unsigned long)honeypot_svc_lure_count());
        }
        return;
    }

    /* ---- karma ---- */
    if (s_hl_mode == HL_WIFI_KARMA) {
        karma_svc_tick(now);
        int nl = karma_svc_client_count();
        if (nl != s_hl_last_clients) {
            kon_printf(s_hl_ks, nl > s_hl_last_clients
                ? "[karma] client connected     total=%d\r\n"
                : "[karma] client disconnected  total=%d\r\n", nl);
            s_hl_last_clients = nl;
        }
        int nc = karma_svc_cred_count();
        if (nc > s_hl_last_cred) {
            for (int i = s_hl_last_cred; i < nc; i++) {
                const wifi_portal_cred_t* c = karma_svc_get_cred(i);
                if (c) kon_printf(s_hl_ks, "[karma] +cred #%d  user=\"%.32s\"  pass=\"%.32s\"\r\n",
                                  i + 1, c->user, c->pass);
            }
            s_hl_last_cred = nc;
        }
        /* New SSIDs (auto mode) */
        int ns = karma_svc_ssid_count();
        if (ns > s_hl_last_ssids) {
            kon_printf(s_hl_ks, "[karma] probe seen  total-ssids=%d  probes=%lu\r\n",
                       ns, (unsigned long)karma_svc_probe_count());
            s_hl_last_ssids = ns;
        }
        if (now - s_hl_last_report >= 30000) {
            s_hl_last_report = now;
            kon_printf(s_hl_ks, "[karma] running  clients=%d  creds=%d  ssids=%d  probes=%lu\r\n",
                       nl, nc, ns, (unsigned long)karma_svc_probe_count());
        }
        return;
    }
#  endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
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

/*
 * Run the active attack in the foreground, printing live stats until the user
 * presses 'q', 'Q', Ctrl-C (0x03) or Ctrl-D (0x04).
 * Calls hl_stop() before returning — caller always exits with mode == IDLE.
 */
static void hl_run_foreground(struct konsole* ks) {
    if (s_hl_timeout_ms)
        kon_printf(ks, "(q or Ctrl+C to stop  timeout: %lus)\r\n",
                   (unsigned long)(s_hl_timeout_ms / 1000));
    else
        kon_printf(ks, "(q or Ctrl+C to stop)\r\n");
    uint32_t t0 = s_arch->millis();
    for (;;) {
        uint8_t ibuf[8];
        int n = s_arch->uart_read(0, ibuf, (int)sizeof(ibuf));
        for (int i = 0; i < n; i++) {
            if (ibuf[i] == 'q' || ibuf[i] == 'Q' ||
                ibuf[i] == 0x03 || ibuf[i] == 0x04) {
                hl_stop();
                kon_printf(ks, "\r\n");
                return;
            }
        }
        if (s_hl_timeout_ms && (s_arch->millis() - t0) >= s_hl_timeout_ms) {
            hl_stop();
            kon_printf(ks, "[stopped: timeout]\r\n");
            return;
        }
        hl_tick();
        s_arch->delay_ms(10);
    }
}

/* ---- Sniff packet callback helpers ------------------------------------ */
#ifdef BS_HAS_WIFI

static bool sniff_mac_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
    return true;
}

static bool sniff_parse_mac(const char* s, uint8_t mac[6]) {
    int v[6] = {0};
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
        return true;
    }
    return false;
}

static const char* sniff_fc_str(uint8_t fc0) {
    uint8_t t = (fc0 >> 2) & 0x03, s = (fc0 >> 4) & 0x0F;
    if (t == 0) {
        switch (s) {
            case 0:  return "assoc-req";   case 1: return "assoc-resp";
            case 2:  return "reassoc-req"; case 3: return "reassoc-resp";
            case 4:  return "probe-req";   case 5: return "probe-resp";
            case 8:  return "beacon";      case 10: return "disassoc";
            case 11: return "auth";        case 12: return "deauth";
            default: return "mgmt";
        }
    }
    if (t == 1) {
        switch (s) {
            case 10: return "ps-poll"; case 11: return "rts";
            case 12: return "cts";     case 13: return "ack";
            default: return "ctrl";
        }
    }
    if (t == 2) {
        switch (s) {
            case 0: return "data"; case 4: return "null-data";
            case 8: return "qos-data"; default: return "data";
        }
    }
    return "ext";
}

static void sniff_hex_dump(struct konsole* ks, const uint8_t* d, uint16_t len) {
    char line[80];
    for (uint16_t off = 0; off < len; off += 16) {
        uint16_t n = (len - off < 16) ? (len - off) : 16;
        int p = 0;
        p += snprintf(line+p, sizeof(line)-p, "  %04x  ", off);
        for (uint16_t i = 0; i < 16; i++) {
            if (i == 8) line[p++] = ' ';
            if (i < n) p += snprintf(line+p, sizeof(line)-p, "%02x ", d[off+i]);
            else       p += snprintf(line+p, sizeof(line)-p, "   ");
        }
        p += snprintf(line+p, sizeof(line)-p, " |");
        for (uint16_t i = 0; i < n; i++) {
            uint8_t c = d[off+i];
            line[p++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[p++] = '|'; line[p] = '\0';
        kon_printf(ks, "%s\r\n", line);
    }
}

static void sniff_pcap_open(const char* path) {
    s_sniff_ctx.pcap_f = bs_fs_open(path, "w");
    if (!s_sniff_ctx.pcap_f) return;
    /* little-endian pcap global header: magic=0xa1b2c3d4  DLT_IEEE802_11=105 */
    static const uint8_t k_hdr[24] = {
        0xd4,0xc3,0xb2,0xa1,  /* magic */
        0x02,0x00, 0x04,0x00, /* ver 2.4 */
        0x00,0x00,0x00,0x00,  /* tz */
        0x00,0x00,0x00,0x00,  /* sigfigs */
        0xff,0xff,0x00,0x00,  /* snaplen 65535 */
        0x69,0x00,0x00,0x00,  /* DLT_IEEE802_11 */
    };
    bs_fs_write(s_sniff_ctx.pcap_f, k_hdr, 24);
    s_sniff_ctx.pcap_count = 0;
}

static void sniff_pcap_write_pkt(const uint8_t* data, uint16_t len, uint32_t ts_ms) {
    if (!s_sniff_ctx.pcap_f) return;
    uint32_t hdr[4];
    hdr[0] = ts_ms / 1000;
    hdr[1] = (ts_ms % 1000) * 1000;
    hdr[2] = hdr[3] = (uint32_t)len;
    bs_fs_write(s_sniff_ctx.pcap_f, hdr, 16);
    bs_fs_write(s_sniff_ctx.pcap_f, data, len);
    s_sniff_ctx.pcap_count++;
}

static void sniff_pkt_cb(const uint8_t* data, uint16_t len,
                         int8_t rssi, uint32_t ts_ms, void* ctx) {
    sniff_ctx_t* c = (sniff_ctx_t*)ctx;
    if (len < 4) return;

    uint8_t fc0  = data[0];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t sub  = (fc0 >> 4) & 0x0F;

    /* frame-type filter */
    if (c->filt_type != SNIFF_FTYPE_ALL) {
        if (type != c->filt_type) return;
        if (c->filt_subtype != SNIFF_FTYPE_ALL && sub != c->filt_subtype) return;
    }

    /* address filters — requires full 24B MAC header */
    if (len >= 16) {
        if (c->has_dst && !sniff_mac_eq(data + 4,  c->filt_dst))   return;
        if (c->has_src && !sniff_mac_eq(data + 10, c->filt_src))   return;
    }
    if (len >= 22) {
        if (c->has_bssid && !sniff_mac_eq(data + 16, c->filt_bssid)) return;
    }

    /* pcap write */
    if (c->pcap_f) sniff_pcap_write_pkt(data, len, ts_ms);

    /* verbose console output */
    if (c->verbose) {
        char m1[18]="?",m2[18]="?",m3[18]="?";
        if (len >= 16) {
            bs_wifi_bssid_str(data + 4,  m1);
            bs_wifi_bssid_str(data + 10, m2);
        }
        if (len >= 22) bs_wifi_bssid_str(data + 16, m3);
        kon_printf(c->ks, "[%s] len=%u rssi=%d ts=%lums\r\n"
                          "  dst=%s  src=%s  bssid=%s\r\n",
                   sniff_fc_str(fc0), (unsigned)len, (int)rssi,
                   (unsigned long)ts_ms, m1, m2, m3);
        sniff_hex_dump(c->ks, data, len);
    }
}

/* ---- Passive scan accumulator (wifi scan) ----------------------------- */
#define SCAN_MAX_APS 64
typedef struct {
    uint8_t bssid[6];
    char    ssid[33];
    uint8_t channel;
    int8_t  rssi;
    int     count;
} scan_ap_t;
static scan_ap_t s_scan_aps[SCAN_MAX_APS];
static int       s_scan_ap_count;

static void scan_beacon_cb(const uint8_t* data, uint16_t len,
                            int8_t rssi, uint32_t ts_ms, void* ctx) {
    (void)ts_ms; (void)ctx;
    if (len < 38) return;
    uint8_t fc0  = data[0];
    uint8_t type = (fc0 >> 2) & 3;
    uint8_t sub  = (fc0 >> 4) & 0xF;
    /* Accept beacons (subtype 8) and probe responses (subtype 5) */
    if (type != 0 || (sub != 8 && sub != 5)) return;
    if (len < 22) return;
    const uint8_t* bssid = data + 16;   /* Addr3 */
    int idx = -1;
    for (int i = 0; i < s_scan_ap_count; i++)
        if (memcmp(s_scan_aps[i].bssid, bssid, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_scan_ap_count >= SCAN_MAX_APS) return;
        idx = s_scan_ap_count++;
        memcpy(s_scan_aps[idx].bssid, bssid, 6);
        s_scan_aps[idx].ssid[0] = '\0';
        s_scan_aps[idx].channel = 0;
        s_scan_aps[idx].count   = 0;
    }
    s_scan_aps[idx].rssi = rssi;
    s_scan_aps[idx].count++;
    /* Parse IEs: beacons and probe-resps both have 12B fixed fields after 24B MAC header */
    int p = 36;
    bool got_ssid = (s_scan_aps[idx].ssid[0] != '\0');
    bool got_ch   = (s_scan_aps[idx].channel  != 0);
    while (p + 2 <= (int)len && !(got_ssid && got_ch)) {
        uint8_t id = data[p], ilen = data[p + 1];
        if (p + 2 + ilen > (int)len) break;
        if (!got_ssid && id == 0 && ilen > 0 && ilen <= 32) {
            memcpy(s_scan_aps[idx].ssid, data + p + 2, ilen);
            s_scan_aps[idx].ssid[ilen] = '\0';
            got_ssid = true;
        } else if (!got_ch && id == 3 && ilen == 1) {
            s_scan_aps[idx].channel = data[p + 2];
            got_ch = true;
        }
        p += 2 + ilen;
    }
}

#endif /* BS_HAS_WIFI (sniff helpers) */

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
            case HL_WIFI_EAPOL:
                kon_printf(ks, "Active: eapol  eapol=%lu  pairs=%d  ch=%u\r\n",
                           (unsigned long)eapol_svc_eapol_count(),
                           eapol_svc_pair_count(),
                           (unsigned)sniffer_svc_channel()); break;
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
            case HL_WIFI_CAPTIVE:
                kon_printf(ks, "Active: captive portal  clients=%d  creds=%d\r\n",
                           captive_svc_client_count(), captive_svc_cred_count()); break;
            case HL_WIFI_EVILTWIN:
                kon_printf(ks, "Active: evil twin  clients=%d  creds=%d  deauth=%lu\r\n",
                           eviltwin_svc_client_count(), eviltwin_svc_cred_count(),
                           (unsigned long)eviltwin_svc_deauth_total()); break;
            case HL_WIFI_HONEYPOT:
                kon_printf(ks, "Active: honeypot  clients=%d  creds=%d  lures=%lu\r\n",
                           honeypot_svc_client_count(), honeypot_svc_cred_count(),
                           (unsigned long)honeypot_svc_lure_count()); break;
            case HL_WIFI_KARMA:
                kon_printf(ks, "Active: karma  clients=%d  creds=%d  probes=%lu  ssids=%d\r\n",
                           karma_svc_client_count(), karma_svc_cred_count(),
                           (unsigned long)karma_svc_probe_count(),
                           karma_svc_ssid_count()); break;
#endif
            default:
                kon_printf(ks, "Idle.\r\n"); break;
        }
        return 0;
    }

    const char* sub = argv[1];

    /* ---- beacon ---- */
    if (bs_stricmp(sub, "beacon") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) want_help = true;
        if (want_help) {
            kon_printf(ks,
                "wifi beacon [OPTIONS]\r\n"
                "  Flood the air with spoofed beacon frames on rotating channels.\r\n"
                "Options:\r\n"
                "  --mode  random|custom|file   SSID source (default: random)\r\n"
                "  --prefix NAME                fixed SSID prefix (implies custom mode)\r\n");
            kon_printf(ks,
                "  --charset ascii|hi|kata|cyr  SSID character set (default: ascii)\r\n"
                "  --repeat N                   TX bursts per SSID 1-20 (default: 3)\r\n"
                "  Channels rotate randomly; no fixed channel option.\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
        beacon_svc_mode_t  mode    = BEACON_MODE_RANDOM;
        wifi_charset_t     charset = WIFI_CHARSET_ASCII;
        char               prefix[21] = "testAP";
        int                repeat  = 3;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--mode") && i+1<argc) {
                if      (!strcmp(argv[i+1],"custom")) mode=BEACON_MODE_CUSTOM;
                else if (!strcmp(argv[i+1],"file"))   mode=BEACON_MODE_FILE;
                else                                  mode=BEACON_MODE_RANDOM;
                i++;
            } else if (!strcmp(argv[i],"--prefix") && i+1<argc) {
                strncpy(prefix,argv[i+1],sizeof prefix-1); prefix[sizeof prefix-1]='\0';
                mode=BEACON_MODE_CUSTOM; i++;
            } else if (!strcmp(argv[i],"--charset") && i+1<argc) {
                if      (!strcmp(argv[i+1],"hi"))   charset=WIFI_CHARSET_HIRAGANA;
                else if (!strcmp(argv[i+1],"kata")) charset=WIFI_CHARSET_KATAKANA;
                else if (!strcmp(argv[i+1],"cyr"))  charset=WIFI_CHARSET_CYRILLIC;
                else                                charset=WIFI_CHARSET_ASCII;
                i++;
            } else if (!strcmp(argv[i],"--repeat") && i+1<argc) {
                int r=atoi(argv[i+1]); if(r>=1&&r<=20) repeat=r; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        beacon_svc_init(s_arch);
        beacon_svc_start(mode, charset, prefix, repeat, NULL, 0);
        s_hl_mode=HL_WIFI_BEACON; s_hl_ks=ks; s_hl_last_report=0;
        kon_printf(ks,"[beacon] mode=%s  charset=%s  repeat=%d\r\n",
                   mode==BEACON_MODE_CUSTOM?"custom":mode==BEACON_MODE_FILE?"file":"random",
                   k_wifi_charset_names[charset], repeat);
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- deauth ---- */
    if (bs_stricmp(sub, "deauth") == 0) {
        /* deauth scan — one-shot AP discovery, no attack */
        if (argc >= 3 && bs_stricmp(argv[2], "scan") == 0) {
            if (s_hl_mode != HL_IDLE) hl_stop();
            if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
            deauth_svc_init(s_arch);
            deauth_svc_scan_aps();
            kon_printf(ks, "Scanning for APs... (q to abort)\r\n");
            bool aborted = false;
            for (;;) {
                uint8_t ibuf[4];
                int n = s_arch->uart_read(0, ibuf, (int)sizeof(ibuf));
                for (int i = 0; i < n; i++) {
                    if (ibuf[i]=='q'||ibuf[i]=='Q'||ibuf[i]==0x03) { aborted=true; break; }
                }
                if (aborted) break;
                deauth_svc_tick(s_arch->millis());
                if (deauth_svc_state() != DEAUTH_SVC_SCANNING) break;
                s_arch->delay_ms(10);
            }
            if (aborted) { kon_printf(ks, "Aborted.\r\n"); }
            else {
                int n = deauth_svc_ap_count();
                if (n == 0) { kon_printf(ks, "No APs found.\r\n"); }
                else {
                    kon_printf(ks, "%d AP(s) found:\r\n", n);
                    kon_printf(ks, "  #   ch  rssi  BSSID              auth       SSID\r\n");
                    for (int i = 0; i < n; i++) {
                        const wifi_ap_entry_t* ap = deauth_svc_ap(i);
                        if (!ap) continue;
                        char mac[18]; bs_wifi_bssid_str(ap->ap.bssid, mac);
                        kon_printf(ks, "  %2d  %2d  %4d  %s  %-10s %s\r\n",
                                   i, ap->ap.channel, (int)ap->ap.rssi,
                                   mac, bs_wifi_auth_str(ap->ap.auth),
                                   ap->ap.ssid[0] ? ap->ap.ssid : "<hidden>");
                    }
                }
            }
            bs_wifi_deinit();
            return 0;
        }

        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi deauth [OPTIONS]         deauth flood (runs until q / Ctrl+C)\r\n"
                "wifi deauth scan              discover APs on air and print table\r\n"
                "Options:\r\n"
                "  --ch N                      channel 1-13 (default: 1)\r\n");
            kon_printf(ks,
                "  --bssid AA:BB:CC:DD:EE:FF   spoof frames from this BSSID\r\n"
                "  --client AA:BB:CC:DD:EE:FF  target specific client (requires --bssid)\r\n"
                "                              bidirectional: AP->client and client->AP\r\n");
            kon_printf(ks,
                "  --reason N                  reason code 1-65535 (default: rotating)\r\n");
            kon_printf(ks,
                "                              1=unspecified  2=prev-auth-invalid\r\n"
                "                              3=leaving      4=inactivity  5=AP-busy\r\n"
                "                              6=class2-nonauth  7=class3-nonassoc\r\n");
            kon_printf(ks,
                "  No --bssid:           broadcast flood, random spoofed src\r\n"
                "  --bssid only:         targeted flood from that AP\r\n"
                "  --bssid + --client:   single-client bidirectional deauth\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
        uint8_t  channel     = 1;
        uint8_t  bssid[6];
        uint8_t  client_mac[6];
        bool     has_bssid   = false;
        bool     has_client  = false;
        uint16_t reason_code = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ch") && i+1<argc) {
                int ch=atoi(argv[i+1]); if(ch>=1&&ch<=13) channel=(uint8_t)ch; i++;
            } else if (!strcmp(argv[i],"--bssid") && i+1<argc) {
                int v[6];
                if (sscanf(argv[i+1],"%x:%x:%x:%x:%x:%x",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6) {
                    for (int b=0;b<6;b++) bssid[b]=(uint8_t)v[b];
                    has_bssid=true;
                }
                i++;
            } else if (!strcmp(argv[i],"--client") && i+1<argc) {
                int v[6];
                if (sscanf(argv[i+1],"%x:%x:%x:%x:%x:%x",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6) {
                    for (int b=0;b<6;b++) client_mac[b]=(uint8_t)v[b];
                    has_client=true;
                }
                i++;
            } else if (!strcmp(argv[i],"--reason") && i+1<argc) {
                int r=atoi(argv[i+1]); if(r>=1&&r<=65535) reason_code=(uint16_t)r; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (has_client && !has_bssid) {
            kon_printf(ks,"Error: --client requires --bssid. Run 'wifi deauth --help'.\r\n");
            return 1;
        }
        deauth_svc_init(s_arch);
        deauth_svc_set_reason(reason_code);
        if (has_bssid && has_client) {
            char bbuf[18], cbuf[18];
            bs_wifi_bssid_str(bssid, bbuf); bs_wifi_bssid_str(client_mac, cbuf);
            deauth_svc_attack_client(bssid, client_mac, channel);
            kon_printf(ks,"[deauth] client  bssid=%s  client=%s  ch=%u%s\r\n",
                       bbuf, cbuf, channel, reason_code?" reason=":"");
            if (reason_code) kon_printf(ks,"  reason=%u\r\n", reason_code);
        } else if (has_bssid) {
            char mbuf[18]; bs_wifi_bssid_str(bssid, mbuf);
            deauth_svc_attack_bssid(bssid, channel);
            kon_printf(ks,"[deauth] targeted  bssid=%s  ch=%u%s\r\n",
                       mbuf, channel, reason_code?" (fixed reason)":"");
        } else {
            deauth_svc_attack_broadcast(channel);
            kon_printf(ks,"[deauth] broadcast  ch=%u%s\r\n",
                       channel, reason_code?" (fixed reason)":"");
        }
        s_hl_mode=HL_WIFI_DEAUTH; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_log=0;
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- sniff ---- */
    if (bs_stricmp(sub, "sniff") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi sniff [OPTIONS]\r\n"
                "  Passive 802.11 capture with filtering and optional pcap output.\r\n"
                "Channel:\r\n"
                "  --ch N       fixed channel 1-13 (default: auto-hop 1-13)\r\n");
            kon_printf(ks,
                "  --hop-ms N   dwell per channel in ms (default: 500)\r\n"
                "  --timeout N  auto-stop after N seconds\r\n");
            kon_printf(ks,
                "Output:\r\n"
                "  -v / --verbose             per-frame hex+ASCII dump\r\n"
                "  --pcap [FILE]              write .pcap to SD (default: wifi/sniff/sniff.pcap)\r\n"
                "Frame-type filter (last --type wins):\r\n"
                "  --type mgmt|ctrl|data      all frames of that type\r\n");
            kon_printf(ks,
                "  --type beacon    beacons (sub 8)    --type probe   probe-req (4)\r\n"
                "  --type proberesp probe-resp (5)     --type deauth  deauth (12)\r\n"
                "  --type disassoc  disassoc (10)      --type auth    auth (11)\r\n"
                "  --type assoc     assoc-req (0)\r\n");
            kon_printf(ks,
                "Address filters (all must match):\r\n"
                "  --dst  AA:BB:CC:DD:EE:FF   Addr1 (destination)\r\n"
                "  --src  AA:BB:CC:DD:EE:FF   Addr2 (source)\r\n"
                "  --bssid AA:BB:CC:DD:EE:FF  Addr3 (BSSID)\r\n"
                "Stats every 3s (10s verbose). q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }

        memset(&s_sniff_ctx, 0, sizeof s_sniff_ctx);
        s_sniff_ctx.filt_type    = SNIFF_FTYPE_ALL;
        s_sniff_ctx.filt_subtype = SNIFF_FTYPE_ALL;
        s_sniff_ctx.ks           = ks;

        uint8_t     channel   = 0;
        uint32_t    hop_ms    = 500;
        const char* pcap_path = NULL;

        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ch") && i+1<argc) {
                int ch=atoi(argv[i+1]); if(ch>=1&&ch<=13) channel=(uint8_t)ch; i++;
            } else if (!strcmp(argv[i],"--hop-ms") && i+1<argc) {
                int ms=atoi(argv[i+1]); if(ms>=50) hop_ms=(uint32_t)ms; i++;
            } else if (!strcmp(argv[i],"--timeout") && i+1<argc) {
                int s=atoi(argv[i+1]); if(s>0) s_hl_timeout_ms=(uint32_t)s*1000u; i++;
            } else if (!strcmp(argv[i],"--verbose")||!strcmp(argv[i],"-v")) {
                s_sniff_ctx.verbose=true;
            } else if (!strcmp(argv[i],"--pcap")) {
                pcap_path=(i+1<argc&&argv[i+1][0]!='-')?argv[++i]:BS_PATH_SNIFF "/sniff.pcap";
            } else if (!strcmp(argv[i],"--type") && i+1<argc) {
                const char* t=argv[++i];
                if      (bs_stricmp(t,"mgmt")    ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=SNIFF_FTYPE_ALL;}
                else if (bs_stricmp(t,"ctrl")    ==0){s_sniff_ctx.filt_type=1;s_sniff_ctx.filt_subtype=SNIFF_FTYPE_ALL;}
                else if (bs_stricmp(t,"data")    ==0){s_sniff_ctx.filt_type=2;s_sniff_ctx.filt_subtype=SNIFF_FTYPE_ALL;}
                else if (bs_stricmp(t,"beacon")  ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=8; }
                else if (bs_stricmp(t,"probe")   ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=4; }
                else if (bs_stricmp(t,"proberesp")==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=5; }
                else if (bs_stricmp(t,"deauth")  ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=12;}
                else if (bs_stricmp(t,"disassoc")==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=10;}
                else if (bs_stricmp(t,"auth")    ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=11;}
                else if (bs_stricmp(t,"assoc")   ==0){s_sniff_ctx.filt_type=0;s_sniff_ctx.filt_subtype=0; }
                else { kon_printf(ks,"Unknown --type '%s'. Use --help.\r\n",t); return 1; }
            } else if (!strcmp(argv[i],"--src") && i+1<argc) {
                if (sniff_parse_mac(argv[++i],s_sniff_ctx.filt_src)) s_sniff_ctx.has_src=true;
                else { kon_printf(ks,"Bad MAC: %s\r\n",argv[i]); return 1; }
            } else if (!strcmp(argv[i],"--dst") && i+1<argc) {
                if (sniff_parse_mac(argv[++i],s_sniff_ctx.filt_dst)) s_sniff_ctx.has_dst=true;
                else { kon_printf(ks,"Bad MAC: %s\r\n",argv[i]); return 1; }
            } else if (!strcmp(argv[i],"--bssid") && i+1<argc) {
                if (sniff_parse_mac(argv[++i],s_sniff_ctx.filt_bssid)) s_sniff_ctx.has_bssid=true;
                else { kon_printf(ks,"Bad MAC: %s\r\n",argv[i]); return 1; }
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }

        if (pcap_path) {
            if (!bs_fs_available()) { kon_printf(ks,"No SD card — pcap unavailable.\r\n"); return 1; }
            bs_fs_mkdir_p(BS_PATH_SNIFF);
            sniff_pcap_open(pcap_path);
            if (!s_sniff_ctx.pcap_f) { kon_printf(ks,"Failed to open: %s\r\n",pcap_path); return 1; }
        }

        sniffer_svc_init(s_arch);
        sniffer_svc_start(channel, hop_ms, sniff_pkt_cb, &s_sniff_ctx);
        s_hl_mode=HL_WIFI_SNIFF; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_total=0;

        if (channel) kon_printf(ks,"[sniff] ch=%u  fixed",  channel);
        else         kon_printf(ks,"[sniff] auto-hop  hop-ms=%lu", (unsigned long)hop_ms);
        if (s_sniff_ctx.verbose) kon_printf(ks,"  verbose");
        if (s_sniff_ctx.pcap_f)  kon_printf(ks,"  pcap=%s", pcap_path);
        kon_printf(ks,"\r\n");
        if (s_sniff_ctx.filt_type != SNIFF_FTYPE_ALL) {
            static const char* const k_tnames[] = {"mgmt","ctrl","data","?"};
            const char* tn = k_tnames[s_sniff_ctx.filt_type < 3 ? s_sniff_ctx.filt_type : 3];
            if (s_sniff_ctx.filt_subtype == SNIFF_FTYPE_ALL)
                kon_printf(ks,"  filter: type=%s\r\n", tn);
            else
                kon_printf(ks,"  filter: type=%s subtype=%u\r\n", tn, s_sniff_ctx.filt_subtype);
        }
        if (s_sniff_ctx.has_src || s_sniff_ctx.has_dst || s_sniff_ctx.has_bssid) {
            char m[18];
            if (s_sniff_ctx.has_dst)  { bs_wifi_bssid_str(s_sniff_ctx.filt_dst,  m); kon_printf(ks,"  filter: dst=%s\r\n",  m); }
            if (s_sniff_ctx.has_src)  { bs_wifi_bssid_str(s_sniff_ctx.filt_src,  m); kon_printf(ks,"  filter: src=%s\r\n",  m); }
            if (s_sniff_ctx.has_bssid){ bs_wifi_bssid_str(s_sniff_ctx.filt_bssid,m); kon_printf(ks,"  filter: bssid=%s\r\n",m); }
        }
        hl_run_foreground(ks);
        return 0;
    }

#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
    /* ---- captive ---- */
    if (bs_stricmp(sub, "captive") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi captive [OPTIONS]\r\n"
                "  Rogue AP + captive portal; captures submitted credentials.\r\n"
                "Options:\r\n"
                "  --ssid NAME    AP SSID (default: FreeWifi)\r\n"
                "  --ch N         channel 1-13 (default: 1)\r\n"
                "  --pass PSK     WPA2 passphrase (default: open)\r\n");
            kon_printf(ks,
                "  Portal URL:    http://192.168.4.1/\r\n"
                "  Credentials printed live as clients submit the portal form.\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks,"WiFi init failed.\r\n"); return 1; }
        char    ssid[33]     = "FreeWifi";
        uint8_t ch           = 1;
        char    password[64] = "";
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ssid")&&i+1<argc) {
                strncpy(ssid,argv[i+1],32); ssid[32]='\0'; i++;
            } else if (!strcmp(argv[i],"--ch")&&i+1<argc) {
                int c=atoi(argv[i+1]); if(c>=1&&c<=13) ch=(uint8_t)c; i++;
            } else if (!strcmp(argv[i],"--pass")&&i+1<argc) {
                strncpy(password,argv[i+1],63); password[63]='\0'; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (!captive_svc_start(ssid, ch, password[0]?password:NULL)) {
            kon_printf(ks,"Portal start failed.\r\n"); return 1;
        }
        s_hl_mode=HL_WIFI_CAPTIVE; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_cred=0;
        kon_printf(ks,"[captive] ssid=\"%s\"  ch=%u  auth=%s\r\n",
                   ssid, ch, password[0]?"WPA2":"open");
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- eviltwin ---- */
    if (bs_stricmp(sub, "eviltwin") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help || argc < 3) {
            kon_printf(ks,
                "wifi eviltwin --ssid NAME [OPTIONS]\r\n"
                "  Clone a target AP; optionally enables SOTA deauth+probe-inject.\r\n"
                "Required:\r\n"
                "  --ssid NAME                target SSID to clone\r\n");
            kon_printf(ks,
                "Options:\r\n"
                "  --ch N                     channel 1-13 (default: 6)\r\n"
                "  --pass PSK                 WPA2 passphrase (default: open)\r\n");
            kon_printf(ks,
                "  --bssid MAC   enables deauth-triggered client redirect:\r\n"
                "    broadcast deauth (reason 7) from real BSSID every 100ms\r\n");
            kon_printf(ks,
                "    + probe-response injection when clients re-probe our SSID\r\n"
                "    flow: deauth->probe-req->probe-resp->assoc->portal\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks,"WiFi init failed.\r\n"); return 1; }
        char    ssid[33]     = "";
        uint8_t ch           = 6;
        char    password[64] = "";
        uint8_t bssid[6];
        bool    has_bssid    = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ssid")&&i+1<argc) {
                strncpy(ssid,argv[i+1],32); ssid[32]='\0'; i++;
            } else if (!strcmp(argv[i],"--ch")&&i+1<argc) {
                int c=atoi(argv[i+1]); if(c>=1&&c<=13) ch=(uint8_t)c; i++;
            } else if (!strcmp(argv[i],"--pass")&&i+1<argc) {
                strncpy(password,argv[i+1],63); password[63]='\0'; i++;
            } else if (!strcmp(argv[i],"--bssid")&&i+1<argc) {
                int v[6];
                if (sscanf(argv[i+1],"%x:%x:%x:%x:%x:%x",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6) {
                    for(int b=0;b<6;b++) bssid[b]=(uint8_t)v[b];
                    has_bssid=true;
                }
                i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (!ssid[0]) {
            kon_printf(ks,"Error: --ssid is required. Run 'wifi eviltwin --help'.\r\n");
            return 1;
        }
        if (!eviltwin_svc_start(ssid, ch, password[0]?password:NULL,
                                has_bssid?bssid:NULL)) {
            kon_printf(ks,"Evil twin start failed.\r\n"); return 1;
        }
        s_hl_mode=HL_WIFI_EVILTWIN; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_cred=0;
        kon_printf(ks,"[eviltwin] ssid=\"%s\"  ch=%u  auth=%s  deauth=%s\r\n",
                   ssid, ch, password[0]?"WPA2":"open",
                   has_bssid?"enabled (SOTA chain)":"disabled (add --bssid to enable)");
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- honeypot ---- */
    if (bs_stricmp(sub, "honeypot") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi honeypot --ssid NAME --bssid MAC --ch N [OPTIONS]\r\n"
                "  Clone AP, inject CSA+deauth lures to redirect its clients.\r\n"
                "Required:\r\n"
                "  --ssid NAME                real AP SSID to clone\r\n");
            kon_printf(ks,
                "  --bssid AA:BB:CC:DD:EE:FF  real AP BSSID\r\n"
                "  --ch N                     real AP channel 1-13\r\n");
            kon_printf(ks,
                "Options:\r\n"
                "  --hpch N                   rogue AP channel (default: auto {1,6,11})\r\n"
                "  --mode broadcast|targeted  all clients or per-client deauth\r\n"
                "  CSA+deauth lures injected every 8s to redirect clients.\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks,"WiFi init failed.\r\n"); return 1; }
        char    target_ssid[33] = "";
        uint8_t target_bssid[6];
        bool    has_bssid       = false;
        uint8_t target_ch       = 0;
        uint8_t hp_ch           = 0;
        honeypot_svc_mode_t hpmode = HONEYPOT_SVC_BROADCAST;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ssid")&&i+1<argc) {
                strncpy(target_ssid,argv[i+1],32); target_ssid[32]='\0'; i++;
            } else if (!strcmp(argv[i],"--bssid")&&i+1<argc) {
                int v[6];
                if (sscanf(argv[i+1],"%x:%x:%x:%x:%x:%x",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6) {
                    for(int b=0;b<6;b++) target_bssid[b]=(uint8_t)v[b];
                    has_bssid=true;
                }
                i++;
            } else if (!strcmp(argv[i],"--ch")&&i+1<argc) {
                int c=atoi(argv[i+1]); if(c>=1&&c<=13) target_ch=(uint8_t)c; i++;
            } else if (!strcmp(argv[i],"--hpch")&&i+1<argc) {
                int c=atoi(argv[i+1]); if(c>=1&&c<=13) hp_ch=(uint8_t)c; i++;
            } else if (!strcmp(argv[i],"--mode")&&i+1<argc) {
                if (bs_stricmp(argv[i+1],"targeted")==0) hpmode=HONEYPOT_SVC_TARGETED; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (!target_ssid[0]||!has_bssid||target_ch==0) {
            kon_printf(ks,"Error: --ssid, --bssid and --ch are required. Run 'wifi honeypot --help'.\r\n");
            return 1;
        }
        if (!honeypot_svc_start(target_ssid, target_bssid, target_ch, hp_ch, hpmode)) {
            kon_printf(ks,"Honeypot start failed.\r\n"); return 1;
        }
        s_hl_mode=HL_WIFI_HONEYPOT; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_cred=0;
        kon_printf(ks,"[honeypot] ssid=\"%s\"  real-ch=%u -> rogue-ch=%u  mode=%s\r\n",
                   target_ssid, target_ch,
                   hp_ch?hp_ch:(uint8_t)(target_ch!=1?1:6),
                   hpmode==HONEYPOT_SVC_TARGETED?"targeted":"broadcast");
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- karma ---- */
    if (bs_stricmp(sub, "karma") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi karma [OPTIONS]\r\n"
                "  Rogue AP answering probe requests; captures portal credentials.\r\n"
                "  Hops all 13 channels for probes; returns to AP ch to serve.\r\n");
            kon_printf(ks,
                "Options:\r\n"
                "  --ssid NAME  SSID to advertise (default: FreeWifi)\r\n"
                "  --ch N       AP channel 1-13 (default: 1)\r\n");
            kon_printf(ks,
                "  --auto       mirror every probed SSID, collect all clients\r\n"
                "  Without --auto: respond only to probes matching --ssid.\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks,"WiFi init failed.\r\n"); return 1; }
        char    ssid[33]  = "FreeWifi";
        uint8_t ch        = 1;
        bool    auto_mode = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ssid")&&i+1<argc) {
                strncpy(ssid,argv[i+1],32); ssid[32]='\0'; i++;
            } else if (!strcmp(argv[i],"--ch")&&i+1<argc) {
                int c=atoi(argv[i+1]); if(c>=1&&c<=13) ch=(uint8_t)c; i++;
            } else if (!strcmp(argv[i],"--auto")) {
                auto_mode=true;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (!karma_svc_start(ssid, ch, auto_mode)) {
            kon_printf(ks,"Karma start failed.\r\n"); return 1;
        }
        s_hl_mode=HL_WIFI_KARMA; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_cred=0;
        kon_printf(ks,"[karma] ssid=\"%s\"  ch=%u  mode=%s\r\n",
                   ssid, ch, auto_mode?"auto (all probe SSIDs)":"specific SSID");
        hl_run_foreground(ks);
        return 0;
    }
#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */

    /* ---- eapol ---- */
    if (bs_stricmp(sub, "eapol") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "wifi eapol [OPTIONS]\r\n"
                "  Passive WPA2 4-way handshake capture via 802.11 monitor mode.\r\n"
                "  pcap output is compatible with hashcat -m 22000 and aircrack-ng.\r\n");
            kon_printf(ks,
                "Options:\r\n"
                "  --bssid MAC    filter to this AP BSSID (default: all)\r\n"
                "  --ch N         fixed channel 1-13 (default: auto-hop)\r\n"
                "  --deauth       send broadcast deauth to trigger reauth (needs --bssid)\r\n");
            kon_printf(ks,
                "  --ivl N        deauth interval ms (default: 5000)\r\n"
                "  --pcap [FILE]  write pcap to SD (default: wifi/eapol/eapol.pcap)\r\n"
                "  --timeout N    auto-stop after N seconds\r\n");
            kon_printf(ks,
                "  Pair = M1 (AP->STA) + M2 (STA->AP) both captured.\r\n"
                "  Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks,"WiFi init failed.\r\n"); return 1; }

        uint8_t     channel    = 0;
        uint8_t     bssid[6];
        bool        has_bssid  = false;
        bool        do_deauth  = false;
        uint32_t    deauth_ivl = 5000;
        const char* pcap_path  = NULL;

        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--ch") && i+1<argc) {
                int ch=atoi(argv[i+1]); if(ch>=1&&ch<=13) channel=(uint8_t)ch; i++;
            } else if (!strcmp(argv[i],"--bssid") && i+1<argc) {
                int v[6];
                if (sscanf(argv[i+1],"%x:%x:%x:%x:%x:%x",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5])==6) {
                    for(int b=0;b<6;b++) bssid[b]=(uint8_t)v[b];
                    has_bssid=true;
                }
                i++;
            } else if (!strcmp(argv[i],"--deauth")) {
                do_deauth=true;
            } else if (!strcmp(argv[i],"--ivl") && i+1<argc) {
                int ms=atoi(argv[i+1]); if(ms>=100) deauth_ivl=(uint32_t)ms; i++;
            } else if (!strcmp(argv[i],"--pcap")) {
                pcap_path=(i+1<argc&&argv[i+1][0]!='-')?argv[++i]:BS_PATH_EAPOL "/eapol.pcap";
            } else if (!strcmp(argv[i],"--timeout") && i+1<argc) {
                int s=atoi(argv[i+1]); if(s>0) s_hl_timeout_ms=(uint32_t)s*1000u; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]);
                return 1;
            }
        }
        if (do_deauth && !has_bssid) {
            kon_printf(ks,"Error: --deauth requires --bssid.\r\n"); return 1;
        }
        if (pcap_path && !bs_fs_available()) {
            kon_printf(ks,"No SD card -- pcap unavailable.\r\n"); return 1;
        }
        if (pcap_path) bs_fs_mkdir_p(BS_PATH_EAPOL);
        eapol_svc_init(s_arch);
        if (!eapol_svc_start(channel, has_bssid?bssid:NULL,
                             do_deauth, deauth_ivl, pcap_path)) {
            kon_printf(ks,"EAPOL capture start failed.\r\n"); return 1;
        }
        s_hl_mode=HL_WIFI_EAPOL; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_clients=0;
        char bbuf[18] = "any";
        if (has_bssid) bs_wifi_bssid_str(bssid, bbuf);
        kon_printf(ks,"[eapol] bssid=%s  ch=%s\r\n",
                   bbuf, channel?"fixed":"auto-hop");
        if (do_deauth)
            kon_printf(ks,"[eapol] deauth every %lums (reason 7)\r\n",
                       (unsigned long)deauth_ivl);
        if (pcap_path) kon_printf(ks,"[eapol] pcap=%s\r\n", pcap_path);
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- scan ---- */
    if (bs_stricmp(sub, "scan") == 0) {
        bool want_help = false;
        uint32_t timeout_s = 10;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) { want_help=true; break; }
            else if (!strcmp(argv[i],"--timeout") && i+1<argc) {
                int s=atoi(argv[i+1]); if(s>0) timeout_s=(uint32_t)s; i++;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        if (want_help) {
            kon_printf(ks,
                "wifi scan [OPTIONS]\r\n"
                "  Passive AP discovery: hops all channels, collects beacons and probe responses.\r\n"
                "Options:\r\n"
                "  --timeout N  scan duration in seconds (default: 10)\r\n"
                "Results sorted by signal strength (RSSI) when done.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_wifi_init(s_arch) < 0) { kon_printf(ks, "WiFi init failed.\r\n"); return 1; }
        s_scan_ap_count = 0;
        memset(s_scan_aps, 0, sizeof s_scan_aps);
        sniffer_svc_init(s_arch);
        sniffer_svc_start(0, 500, scan_beacon_cb, NULL);
        kon_printf(ks,"[scan] passive AP discovery  timeout=%lus\r\n",(unsigned long)timeout_s);
        uint32_t t0 = s_arch->millis();
        bool aborted = false;
        for (;;) {
            uint8_t ibuf[4];
            int n = s_arch->uart_read(0, ibuf, (int)sizeof(ibuf));
            for (int i = 0; i < n; i++) {
                if (ibuf[i]=='q'||ibuf[i]=='Q'||ibuf[i]==0x03) { aborted=true; break; }
            }
            if (aborted) break;
            sniffer_svc_tick(s_arch->millis());
            if ((s_arch->millis() - t0) >= timeout_s * 1000u) break;
            s_arch->delay_ms(10);
        }
        sniffer_svc_stop();
        bs_wifi_deinit();
        int count = s_scan_ap_count;
        if (aborted) kon_printf(ks,"Aborted. ");
        kon_printf(ks,"%d AP(s) found:\r\n", count);
        if (count > 0) {
            /* Bubble-sort by RSSI descending */
            for (int i = 0; i < count - 1; i++)
                for (int j = i + 1; j < count; j++)
                    if (s_scan_aps[j].rssi > s_scan_aps[i].rssi) {
                        scan_ap_t tmp = s_scan_aps[i];
                        s_scan_aps[i] = s_scan_aps[j];
                        s_scan_aps[j] = tmp;
                    }
            kon_printf(ks,"  #   ch  rssi  pkts  BSSID              SSID\r\n");
            for (int i = 0; i < count; i++) {
                char mac[18]; bs_wifi_bssid_str(s_scan_aps[i].bssid, mac);
                kon_printf(ks,"  %2d  %2d  %4d  %4d  %s  %s\r\n",
                           i, s_scan_aps[i].channel, (int)s_scan_aps[i].rssi,
                           s_scan_aps[i].count,
                           mac,
                           s_scan_aps[i].ssid[0] ? s_scan_aps[i].ssid : "<hidden>");
            }
        }
        return 0;
    }

    /* Unknown subcommand — error unless it's a help flag */
    if (strcmp(sub,"-h")!=0 && strcmp(sub,"--help")!=0) {
        kon_printf(ks,"Unknown wifi subcommand: %s\r\nRun 'wifi' for help.\r\n", sub);
        return 1;
    }
    kon_printf(ks,
        "wifi - 802.11 attack toolset\r\n"
        "  wifi scan     [--help]  passive AP discovery\r\n"
        "  wifi beacon   [--help]  beacon spam, rotating channels\r\n"
        "  wifi deauth   [--help]  deauth flood - broadcast/targeted\r\n"
        "  wifi deauth   scan      active AP scan (no attack)\r\n");
    kon_printf(ks,
        "  wifi sniff    [--help]  passive capture, filters, pcap, timeout\r\n"
        "  wifi eapol    [--help]  WPA2 handshake capture (+ deauth trigger)\r\n"
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
        "  wifi captive  [--help]  rogue AP + captive portal\r\n"
        "  wifi eviltwin [--help]  clone AP + deauth-triggered redirect chain\r\n"
#endif
        );
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)
    kon_printf(ks,
        "  wifi honeypot [--help]  clone AP + CSA/deauth lures\r\n"
        "  wifi karma    [--help]  probe-response rogue AP (13ch hop)\r\n"
        "  wifi          show attack status and caps\r\n"
        "Each command blocks; q / Ctrl+C to stop.\r\n");
#else
    kon_printf(ks,
        "  wifi          show attack status and caps\r\n"
        "Each command blocks; q / Ctrl+C to stop.\r\n");
#endif
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

    const char* sub = argv[1];

    /* ---- spam ---- */
    if (bs_stricmp(sub, "spam") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "ble spam [OPTIONS]\r\n"
                "  Flood BLE advert space with spoofed vendor frames.\r\n"
                "Options:\r\n"
                "  --mode apple|samsung|google|name|all  advert type (default: all)\r\n"
                "  --ivl N   interval ms (default: 100)\r\n"
                "  Stats every 5s. q / Ctrl+C to stop.\r\n");
            return 0;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_ble_init(s_arch) < 0) { kon_printf(ks,"BLE init failed.\r\n"); return 1; }
        ble_spam_mode_t mode     = BLE_SPAM_ALL;
        uint32_t        interval = 100;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--mode")&&i+1<argc) {
                const char* m=argv[++i];
                if      (bs_stricmp(m,"apple")  ==0) mode=BLE_SPAM_APPLE;
                else if (bs_stricmp(m,"samsung")==0) mode=BLE_SPAM_SAMSUNG;
                else if (bs_stricmp(m,"google") ==0) mode=BLE_SPAM_GOOGLE;
                else if (bs_stricmp(m,"name")   ==0) mode=BLE_SPAM_NAME;
                else                                  mode=BLE_SPAM_ALL;
            } else if (!strcmp(argv[i],"--ivl")&&i+1<argc) {
                int ms=atoi(argv[++i]); if(ms>0) interval=(uint32_t)ms;
            } else {
                kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
            }
        }
        ble_spam_svc_init(s_arch);
        ble_spam_svc_start(mode, interval);
        s_hl_mode=HL_BLE_SPAM; s_hl_ks=ks; s_hl_last_report=0;
        kon_printf(ks,"[ble-spam] mode=%s  ivl=%lums\r\n",
                   k_ble_spam_mode_names[mode],(unsigned long)interval);
        hl_run_foreground(ks);
        return 0;
    }

    /* ---- scan ---- */
    if (bs_stricmp(sub, "scan") == 0) {
        bool want_help = false;
        for (int i = 2; i < argc && !want_help; i++)
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) want_help=true;
        if (want_help) {
            kon_printf(ks,
                "ble scan\r\n"
                "  Passive BLE device discovery.\r\n"
                "  New device count printed every 3s. Press q / Ctrl+C to stop.\r\n");
            return 0;
        }
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) continue;
            kon_printf(ks,"Unknown argument: %s  (run with --help)\r\n",argv[i]); return 1;
        }
        if (s_hl_mode != HL_IDLE) hl_stop();
        if (bs_ble_init(s_arch) < 0) { kon_printf(ks,"BLE init failed.\r\n"); return 1; }
        ble_scan_svc_init(s_arch);
        ble_scan_svc_start();
        s_hl_mode=HL_BLE_SCAN; s_hl_ks=ks; s_hl_last_report=0; s_hl_last_total=0;
        kon_printf(ks,"[ble-scan] scanning...\r\n");
        hl_run_foreground(ks);
        return 0;
    }

    /* Unknown subcommand — error unless it's a help flag */
    if (strcmp(sub,"-h")!=0 && strcmp(sub,"--help")!=0) {
        kon_printf(ks,"Unknown ble subcommand: %s\r\nRun 'ble' for help.\r\n", sub);
        return 1;
    }
    kon_printf(ks,
        "ble - Bluetooth Low Energy toolset\r\n"
        "  ble spam [--help]  advert flood (Apple/Samsung/Google/name)\r\n"
        "  ble scan [--help]  passive BLE device discovery\r\n"
        "  ble                show active BLE status and caps\r\n"
        "Each command blocks; q / Ctrl+C to stop.\r\n");
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
 * Keys: text_scale  brightness  layout  palette  border  voltage  header_brand
 */

/* Palette / border names come from bs_theme — declare enough for lookup */
static int cmd_opts(struct konsole* ks, int argc, char** argv) {
    /* opts sdinfo */
    if (argc >= 2 && bs_stricmp(argv[1], "sdinfo") == 0) {
        if (bs_fs_available()) {
            long log_sz = bs_fs_file_size(BS_PATH_LOG);
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
        int n = snprintf(buf, sizeof buf,
            "layout=%d\ngrid_cols=%d\ngrid_rows=%d\npalette=%d\nborder=%d\n"
            "text_scale=%.1f\nbrightness=%d\nshow_voltage=%d\ncarousel=%d\nheader_brand=%d\n",
            (int)bs_menu_get_mode(),
            bs_ui_grid_max_cols(), bs_ui_grid_max_rows(),
            bs_ui_palette_idx(), bs_ui_border_idx(),
            (double)bs_ui_text_scale(), bs_ui_brightness(),
            (int)bs_ui_show_voltage(),
            (int)bs_ui_carousel_enabled(), bs_ui_header_brand_mode());
        bs_fs_write_file(BS_PATH_SETTINGS, buf, (size_t)n);
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
        if (bs_stricmp(key, "header_brand") == 0) {
            int mode = -1;
            if      (bs_stricmp(val, "text") == 0) mode = 0;
            else if (bs_stricmp(val, "logo") == 0) mode = 1;
            else mode = atoi(val);
            bs_ui_set_header_brand_mode(mode);
            bs_menu_invalidate();
            kon_printf(ks, "header_brand=%s\r\n", bs_ui_header_brand_mode() ? "logo" : "text");
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
        if (bs_stricmp(key, "palette") == 0) {
            int idx = atoi(val);
            if (idx < 0 || idx >= BS_PALETTE_COUNT) {
                kon_printf(ks, "palette: 0-%d\r\n", BS_PALETTE_COUNT - 1); return 1;
            }
            bs_ui_set_palette_idx(idx);
            bs_theme_apply(idx, (bs_border_style_t)bs_ui_border_idx());
            bs_menu_invalidate();
            kon_printf(ks, "palette=%d\r\n", idx);
            return 0;
        }
        if (bs_stricmp(key, "border") == 0) {
            int idx = atoi(val);
            if (idx < 0 || idx >= BS_BORDER_STYLE_COUNT) {
                kon_printf(ks, "border: 0-%d\r\n", BS_BORDER_STYLE_COUNT - 1); return 1;
            }
            bs_ui_set_border_idx(idx);
            bs_theme_apply(bs_ui_palette_idx(), (bs_border_style_t)idx);
            bs_menu_invalidate();
            kon_printf(ks, "border=%d\r\n", idx);
            return 0;
        }
        if (bs_stricmp(key, "carousel") == 0) {
            bool on = (bs_stricmp(val,"on")==0 || strcmp(val,"1")==0);
            bs_ui_set_carousel(on);
            kon_printf(ks, "carousel=%s\r\n", on ? "on" : "off");
            return 0;
        }
        kon_printf(ks, "Unknown key '%s'.\r\n", key);
        kon_printf(ks, "Keys: text_scale brightness layout voltage\r\n");
        kon_printf(ks, "      header_brand grid_cols grid_rows\r\n");
        kon_printf(ks, "      palette border carousel\r\n");
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
    kon_printf(ks, "  palette     = %d  border=%d\r\n",
               bs_ui_palette_idx(), bs_ui_border_idx());
    kon_printf(ks, "  carousel    = %s\r\n", bs_ui_carousel_enabled() ? "on" : "off");
    kon_printf(ks, "  voltage     = %s\r\n", bs_ui_show_voltage() ? "on" : "off");
    kon_printf(ks, "  header_brand= %s\r\n", bs_ui_header_brand_mode() ? "logo" : "text");
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
    { "wifi",  "wifi scan|beacon|deauth|sniff|eapol|captive|eviltwin|honeypot|karma [--help]", cmd_wifi },
#endif
#ifdef BS_HAS_BLE
    { "ble",   "ble spam|scan [--help]",  cmd_ble  },
#endif
};

/* ---- bs_init ---------------------------------------------------------- */
void bs_init(void) {
    s_arch = arch_bs();
    s_arch->init();
#ifdef BS_UART_BAUD
    s_arch->uart_init(0, BS_UART_BAUD);
#else
    s_arch->uart_init(0, 115200);
#endif

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

    /* Board bring-up owns target-specific early init (SIC, power rails, quirks). */
    bs_board_init(s_arch);

    /* Filesystem - any board-specific storage power-up has already run. */
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
