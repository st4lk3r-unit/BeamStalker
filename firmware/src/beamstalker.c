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
 *   7. BLE stack (before WiFi — coex controller must start in BT+WiFi mode)
 *   8. WiFi stack
 *   9. SIC hardware (keyboard, audio, battery … - hardware targets only)
 *  10. bs_boot_run()   - animated boot sequence         [skipped if BS_HEADLESS]
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
 *   BS_NO_APP_BLE    - exclude BLE app  (auto-excluded if no BLE backend)
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
#include "bs/bs_ble.h"    /* defines BS_HAS_BLE  when a backend is active */
#if defined(BS_HAS_BLE) && !defined(BS_NO_APP_BLE)
#  include "apps/app_ble.h"
#endif

#include "konsole/konsole.h"
#include "konsole/static.h"   /* struct kon_line_state */
#include "board.h"            /* variant-specific defines (via -I variant/<name>) */

#include <string.h>
#include <stdio.h>

/* ---- Optional SIC ----------------------------------------------------- */
#ifdef BS_USE_SIC
#  include <sic/sic.h>
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

/* ---- Idle callbacks --------------------------------------------------- */
static void menu_idle_poll(void) {
    konsole_poll(&g_ks);
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

static const struct kon_cmd k_cmds[] = {
    { "help",         "show commands",              cmd_help         },
    { "hw",           "hardware & system status",   cmd_hw           },
    { "dmesg",        "show log (dmesg flush->SD)", cmd_dmesg        },
    { "sdformat",     "format SD card as FAT32",    cmd_sdformat     },
    { "startapp",     "launch app by name",         cmd_startapp     },
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

    /* Radio probe — init each stack sequentially to verify hardware, log the
     * result, then deinit.  On ESP32 the coexistence controller aborts if two
     * stacks are active simultaneously, so we never overlap them.  Each app
     * re-inits its own stack on entry and deinits it on BACK.               */
    bool probe_ble_ok  = false;
    bool probe_wifi_ok = false;

#ifdef BS_HAS_BLE
    {
        int berr = bs_ble_init(s_arch);
        probe_ble_ok = (berr == 0);
        if (berr == 0) {
            uint32_t bcaps = bs_ble_caps();
            BS_LOGOK("ble", "caps=0x%02X  (adv=%s scan=%s rand=%s)",
                     (unsigned)bcaps,
                     (bcaps & BS_BLE_CAP_ADVERTISE) ? "Y" : "N",
                     (bcaps & BS_BLE_CAP_SCAN)      ? "Y" : "N",
                     (bcaps & BS_BLE_CAP_RAND_ADDR) ? "Y" : "N");
        } else {
            BS_LOGBF("ble", "probe failed (%d)", berr);
        }
        bs_ble_deinit();
        /* Give the coexistence controller time to settle before WiFi init.
         * Without this delay, esp_wifi_init() fails because the coex state
         * machine is still in BT teardown when WiFi tries to join it.        */
        s_arch->delay_ms(200);
    }
#endif
#ifdef BS_HAS_WIFI
    {
        int werr = bs_wifi_init(s_arch);
        probe_wifi_ok = (werr == 0);
        if (werr == 0) {
            uint32_t wcaps = bs_wifi_caps();
            BS_LOGOK("wifi", "caps=0x%02X  (inject=%s sniff=%s scan=%s)",
                     (unsigned)wcaps,
                     (wcaps & BS_WIFI_CAP_INJECT) ? "Y" : "N",
                     (wcaps & BS_WIFI_CAP_SNIFF)  ? "Y" : "N",
                     (wcaps & BS_WIFI_CAP_SCAN)   ? "Y" : "N");
        } else {
            BS_LOGBF("wifi", "probe failed (%d)", werr);
        }
        bs_wifi_deinit();
    }
#endif

    bs_boot_set_probe(probe_wifi_ok, probe_ble_ok);

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
