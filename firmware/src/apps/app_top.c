/*
 * app_top.c - System performance monitor (sysinfo only).
 *
 * Scrollable table: Board, Battery, CPU, Memory, Flash, Tasks.
 * Memory values refresh every 2 s; battery every 10 s.
 *
 * Navigation:
 *   UP / DOWN → scroll rows
 *   BACK      → exit
 */
#include "apps/app_top.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_hw.h"
#include "bs/bs_debug.h"
#include "bs/bs_assets.h"
#include "bs/bs_ui.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Uptime formatter -------------------------------------------------- */

static void fmt_uptime(char* buf, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t m = s  / 60;  s %= 60;
    uint32_t h = m  / 60;  m %= 60;
    if (h > 0)
        snprintf(buf, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else
        snprintf(buf, n, "%02u:%02u", (unsigned)m, (unsigned)s);
}

/* ---- Sysinfo rows ------------------------------------------------------- */

#define SYSINFO_MAX_ROWS 64

typedef struct {
    char  label[28];
    char  value[64];
    bool  is_section;
    bool  warn;
} sysinfo_row_t;

static sysinfo_row_t s_info_rows[SYSINFO_MAX_ROWS];
static int           s_info_nrows;
static int           s_info_scroll;
static int           s_info_cursor;

/* Dynamic row indices (-1 = absent). */
static int s_row_heap      = -1;
static int s_row_heap_used = -1;
static int s_row_iheap     = -1;
static int s_row_minheap   = -1;
static int s_row_psram     = -1;
static int s_row_bat_pct   = -1;
static int s_row_bat_mv    = -1;

static bool s_sysinfo_built = false;

static void info_sect(const char* name) {
    if (s_info_nrows >= SYSINFO_MAX_ROWS) return;
    sysinfo_row_t* r = &s_info_rows[s_info_nrows++];
    strncpy(r->label, name, sizeof(r->label) - 1);
    r->label[sizeof(r->label)-1] = '\0';
    r->value[0] = '\0';
    r->is_section = true;
    r->warn = false;
}

static void info_row(const char* label, bool warn, const char* fmt, ...) {
    if (s_info_nrows >= SYSINFO_MAX_ROWS) return;
    sysinfo_row_t* r = &s_info_rows[s_info_nrows++];
    strncpy(r->label, label, sizeof(r->label) - 1);
    r->label[sizeof(r->label)-1] = '\0';
    r->is_section = false;
    r->warn = warn;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->value, sizeof(r->value), fmt, ap);
    va_end(ap);
}

static int task_cmp_hwm(const void* a, const void* b) {
    const bs_hw_task_t* ta = (const bs_hw_task_t*)a;
    const bs_hw_task_t* tb = (const bs_hw_task_t*)b;
    if (ta->stack_hwm_b < tb->stack_hwm_b) return -1;
    if (ta->stack_hwm_b > tb->stack_hwm_b) return  1;
    return 0;
}

/*
 * Build the full sysinfo table once.
 * Dynamic rows (memory, battery) record their index for cheap refresh.
 * Tasks section uses the expensive FreeRTOS call — only here, once.
 */
static void build_sysinfo(void) {
    s_info_nrows = 0;
    s_row_heap = s_row_heap_used = s_row_iheap = s_row_minheap = s_row_psram = -1;
    s_row_bat_pct = s_row_bat_mv = -1;

    bs_hw_info_t hw;
    bs_hw_get_info(&hw);

    /* ---- Battery (dynamic) ---- */
    int bpct = bs_hw_battery_pct();
    int bmv  = bs_hw_battery_mv();
    if (bpct > 0 || bmv > 0) {
        info_sect("BATTERY");
        if (bpct > 0) {
            info_row("Level", bpct < 20, "%d%%", bpct);
            s_row_bat_pct = s_info_nrows - 1;
        }
        if (bmv > 0) {
            info_row("Voltage", false, "%d.%02d V", bmv / 1000, (bmv % 1000) / 10);
            s_row_bat_mv = s_info_nrows - 1;
        }
    }

    /* ---- Board ---- */
    info_sect("BOARD");
    if (hw.board && hw.board[0])     info_row("Model",    false, "%s", hw.board);
    if (hw.chip_rev >= 0)            info_row("Chip Rev", false, "%d", hw.chip_rev);
    if (hw.sdk_ver && hw.sdk_ver[0]) info_row("SDK",      false, "%s", hw.sdk_ver);

    /* ---- CPU ---- */
    info_sect("CPU");
    info_row("Frequency", false, "%u MHz", hw.cpu_mhz);
    if (hw.cores > 0)      info_row("Cores", false, "%d", hw.cores);
    if (hw.task_count > 0) info_row("Tasks", false, "%u running", hw.task_count);

    /* ---- Memory (dynamic — granular rows) ---- */
    info_sect("MEMORY");

    /* Heap free */
    info_row("Heap Free", hw.heap_free_kb < 20, "%u kB", hw.heap_free_kb);
    s_row_heap = s_info_nrows - 1;

    /* Heap total (static after boot) */
    if (hw.heap_total_kb) {
        info_row("Heap Total", false, "%u kB", hw.heap_total_kb);
        /* Heap used % (dynamic) */
        uint32_t used = hw.heap_total_kb > hw.heap_free_kb
                        ? hw.heap_total_kb - hw.heap_free_kb : 0;
        uint32_t pct  = hw.heap_total_kb ? used * 100 / hw.heap_total_kb : 0;
        info_row("Heap Used", pct > 85, "%u kB  (%u%%)", used, pct);
        s_row_heap_used = s_info_nrows - 1;
    }

    /* Internal (SRAM-only) heap, if distinct */
    if (hw.heap_internal_kb && hw.heap_internal_kb != hw.heap_free_kb) {
        info_row("Internal Free", false, "%u kB", hw.heap_internal_kb);
        s_row_iheap = s_info_nrows - 1;
    }

    /* Min-ever heap (warn if critically low) */
    info_row("Min Ever", hw.heap_min_kb < 20, "%u kB", hw.heap_min_kb);
    s_row_minheap = s_info_nrows - 1;

    /* PSRAM */
    if (hw.psram_total_kb) {
        info_row("PSRAM Free",  false, "%u kB", hw.psram_free_kb);
        s_row_psram = s_info_nrows - 1;
        info_row("PSRAM Total", false, "%u kB", hw.psram_total_kb);
    }

    /* ---- Flash (static) ---- */
    info_sect("FLASH");
    if (hw.flash_kb)      info_row("Chip Size",   false, "%u kB", hw.flash_kb);
    if (hw.firmware_kb)   info_row("Sketch Size", false, "%u kB", hw.firmware_kb);
    if (hw.flash_free_kb) info_row("Free (OTA)",  false, "%u kB", hw.flash_free_kb);

    /* ---- Tasks (expensive — once only) ---- */
    bs_hw_task_t tasks[BS_HW_MAX_TASKS];
    int ntasks = bs_hw_task_list(tasks, BS_HW_MAX_TASKS);
    if (ntasks > 0) {
        qsort(tasks, (size_t)ntasks, sizeof tasks[0], task_cmp_hwm);
        info_sect("TASKS  (stack headroom)");
        for (int i = 0; i < ntasks; i++) {
            char label[28];
            snprintf(label, sizeof label, "  %s  p%u",
                     tasks[i].name, tasks[i].priority);
            info_row(label, tasks[i].stack_hwm_b < 512, "%u B", tasks[i].stack_hwm_b);
        }
    }

    s_sysinfo_built = true;
}

/* Patch dynamic rows — called every 2 s. */
static void refresh_sysinfo_dyn(void) {
    bs_hw_info_t hw;
    bs_hw_get_info(&hw);

    if (s_row_heap >= 0) {
        sysinfo_row_t* r = &s_info_rows[s_row_heap];
        snprintf(r->value, sizeof r->value, "%u kB", hw.heap_free_kb);
        r->warn = (hw.heap_free_kb < 20);
    }
    if (s_row_heap_used >= 0 && hw.heap_total_kb) {
        uint32_t used = hw.heap_total_kb > hw.heap_free_kb
                        ? hw.heap_total_kb - hw.heap_free_kb : 0;
        uint32_t pct  = used * 100 / hw.heap_total_kb;
        sysinfo_row_t* r = &s_info_rows[s_row_heap_used];
        snprintf(r->value, sizeof r->value, "%u kB  (%u%%)", used, pct);
        r->warn = (pct > 85);
    }
    if (s_row_iheap >= 0)
        snprintf(s_info_rows[s_row_iheap].value,
                 sizeof s_info_rows[s_row_iheap].value,
                 "%u kB", hw.heap_internal_kb);
    if (s_row_minheap >= 0) {
        sysinfo_row_t* r = &s_info_rows[s_row_minheap];
        snprintf(r->value, sizeof r->value, "%u kB", hw.heap_min_kb);
        r->warn = (hw.heap_min_kb < 20);
    }
    if (s_row_psram >= 0)
        snprintf(s_info_rows[s_row_psram].value,
                 sizeof s_info_rows[s_row_psram].value,
                 "%u kB", hw.psram_free_kb);

    /* Battery (refreshed together with memory) */
    if (s_row_bat_pct >= 0) {
        int pct = bs_hw_battery_pct();
        sysinfo_row_t* r = &s_info_rows[s_row_bat_pct];
        snprintf(r->value, sizeof r->value, "%d%%", pct);
        r->warn = (pct > 0 && pct < 20);
    }
    if (s_row_bat_mv >= 0) {
        int mv = bs_hw_battery_mv();
        sysinfo_row_t* r = &s_info_rows[s_row_bat_mv];
        snprintf(r->value, sizeof r->value, "%d.%02d V", mv / 1000, (mv % 1000) / 10);
    }
}

/* ---- Sysinfo draw ------------------------------------------------------- */

static void draw_sysinfo(int sw, int sh, uint32_t now_ms) {
    float ts  = bs_ui_text_scale(); if (ts > 2.0f) ts = 2.0f;
    int rh    = bs_ui_row_h(ts);
    int visible = bs_ui_list_visible(ts);

    bs_ui_list_clamp_scroll(s_info_cursor, &s_info_scroll, s_info_nrows, visible);

    bs_gfx_clear(g_bs_theme.bg);

    char ubuf[16], hbuf[48];
    fmt_uptime(ubuf, sizeof ubuf, now_ms);
    snprintf(hbuf, sizeof hbuf, "Sysinfo  |  up %s", ubuf);
    bs_ui_draw_header(hbuf);

    int content_y = bs_ui_content_y();

    for (int i = 0; i < visible; i++) {
        int ri = i + s_info_scroll;
        if (ri >= s_info_nrows) break;
        const sysinfo_row_t* r = &s_info_rows[ri];
        int row_y = content_y + i * rh;
        bool sel  = (ri == s_info_cursor);

        if (r->is_section) {
            bs_gfx_hline(6, row_y + rh - 2, sw - 12, g_bs_theme.dim);
            bs_gfx_text(6, row_y, r->label, g_bs_theme.accent, ts);
        } else {
            bs_ui_draw_kv_row(row_y, r->label, r->value, sel, r->warn, ts);
        }
    }

    bs_ui_draw_scroll_arrows(s_info_scroll, s_info_nrows, visible);
    bs_ui_draw_hint("up/dn:scroll  back:exit");
}

/* ---- App --------------------------------------------------------------- */

static void top_run(const bs_arch_t* arch) {
    int sw = bs_gfx_width();
    int sh = bs_gfx_height();

    s_info_scroll   = 0;
    s_info_cursor   = 0;
    s_sysinfo_built = false;

    bool dirty = true;
    uint32_t prev_dyn_ms = arch->millis();
    uint32_t prev_ms     = prev_dyn_ms;
    uint32_t last_anim_ms = prev_dyn_ms;

    for (;;) {
        uint32_t now_ms = arch->millis();
        bs_ui_advance_ms(now_ms - prev_ms);
        prev_ms = now_ms;

        if (!s_sysinfo_built) { build_sysinfo(); dirty = true; }

        /* Refresh dynamic rows every 2 s */
        if (now_ms - prev_dyn_ms >= 2000) {
            prev_dyn_ms = now_ms;
            refresh_sysinfo_dyn();
            dirty = true;
        }

        /* Drain all pending nav events */
        bool quit = false;
        bs_nav_id_t nav;
        while (!quit && (nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_BACK:
                    quit = true;
                    break;
                case BS_NAV_UP:
                case BS_NAV_PREV:
                    if (s_info_cursor > 0) { s_info_cursor--; dirty = true; }
                    break;
                case BS_NAV_DOWN:
                case BS_NAV_NEXT:
                    if (s_info_cursor < s_info_nrows - 1) { s_info_cursor++; dirty = true; }
                    break;
                default:
                    break;
            }
        }
        if (quit) return;

        bool anim_due = bs_ui_carousel_enabled() && (uint32_t)(now_ms - last_anim_ms) >= 100U;
        if (dirty || anim_due) {
            draw_sysinfo(sw, sh, now_ms);
            bs_debug_frame();
            bs_gfx_present();
            dirty = false;
            if (anim_due) last_anim_ms = now_ms;
        }
#if defined(VARIANT_TPAGER) || defined(VARIANT_TDONGLE_S3) || defined(VARIANT_HELTEC_V3)
        arch->delay_ms(1);
#else
        arch->delay_ms(2);
#endif
    }
}

const bs_app_t app_top = {
    .name   = "Top",
    .icon   = bs_chart_32,
    .icon_w = 32,
    .icon_h = 32,
    .run    = top_run,
};
