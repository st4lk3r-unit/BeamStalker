/*
 * app_top.c - System performance monitor.
 *
 * Page 0 — GRAPHS
 *   Rolling heap-free KB and frame-time charts.
 *
 * Page 1 — SYSINFO (scrollable)
 *   Board, CPU, Memory, Flash, Tasks (FreeRTOS).
 *   Values that overflow the column scroll (marquee) automatically.
 *
 * Navigation:
 *   Graphs  : RIGHT/SELECT → sysinfo,          BACK → menu
 *   Sysinfo : UP/DOWN → scroll, LEFT → graphs, BACK → menu
 */
#include "apps/app_top.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_keys.h"
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

/* ---- Sample ring (graphs page) ----------------------------------------- */
#define TOP_SAMPLES 512

static uint16_t s_heap_kb [TOP_SAMPLES];
static uint16_t s_frame_ms[TOP_SAMPLES];
static int      s_head;
static int      s_count;

static void ring_push(uint16_t heap, uint16_t fms) {
    s_heap_kb [s_head] = heap;
    s_frame_ms[s_head] = fms;
    s_head = (s_head + 1) % TOP_SAMPLES;
    if (s_count < TOP_SAMPLES) s_count++;
}

/* ---- Graph helper ------------------------------------------------------- */

static void draw_graph(int gx, int gy, int gw, int gh,
                        const uint16_t* ring,
                        const char* title, const char* unit,
                        bs_color_t line_col) {
    float ts    = bs_ui_text_scale();
    int title_h = bs_gfx_text_h(ts) + 3;

    bs_gfx_fill_rect(gx, gy, gw, gh, g_bs_theme.bg);
    bs_gfx_hline   (gx, gy,      gw, g_bs_theme.dim);
    bs_gfx_hline   (gx, gy+gh-1, gw, g_bs_theme.dim);
    bs_gfx_fill_rect(gx,      gy, 1, gh, g_bs_theme.dim);
    bs_gfx_fill_rect(gx+gw-1, gy, 1, gh, g_bs_theme.dim);
    bs_gfx_text(gx + 4, gy + 2, title, g_bs_theme.dim, ts);

    if (s_count < 2) return;

    int px = gx + 2, py = gy + title_h;
    int pw = gw - 4, ph = gh - title_h - 3;
    if (ph < 4 || pw < 4) return;

    int n_plot = s_count < pw ? s_count : pw;

    uint16_t vmin = 0xFFFF, vmax = 0;
    for (int i = 0; i < n_plot; i++) {
        int idx = (s_head - n_plot + i + TOP_SAMPLES) % TOP_SAMPLES;
        uint16_t v = ring[idx];
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    if (vmax == vmin) vmax = vmin + 1;
    uint32_t range = (uint32_t)(vmax - vmin);

    for (int p = 1; p <= 3; p++) {
        int yl = py + ph - (int)((uint32_t)p * (uint32_t)ph / 4);
        for (int xi = px; xi < px + pw; xi += 6)
            bs_gfx_fill_rect(xi, yl, 3, 1, g_bs_theme.dim);
    }

    for (int i = 0; i < n_plot; i++) {
        int idx = (s_head - n_plot + i + TOP_SAMPLES) % TOP_SAMPLES;
        uint16_t val = ring[idx];
        int bar_h = (int)((uint32_t)(val - vmin) * (uint32_t)ph / range);
        if (bar_h < 1) bar_h = 1;
        int bx = px + i;
        if (bar_h > 1)
            bs_gfx_fill_rect(bx, py + ph - bar_h + 1, 1, bar_h - 1, g_bs_theme.dim);
        bs_gfx_fill_rect(bx, py + ph - bar_h, 1, 1, line_col);
    }

    uint16_t cur = ring[(s_head - 1 + TOP_SAMPLES) % TOP_SAMPLES];
    char buf[24];
    snprintf(buf, sizeof buf, "%u%s", (unsigned)cur, unit);
    int lw = bs_gfx_text_w(buf, ts);
    int lx = gx + gw - lw - 5;
    bs_gfx_fill_rect(lx - 2, gy + 2, lw + 4, bs_gfx_text_h(ts), g_bs_theme.bg);
    bs_gfx_text(lx, gy + 2, buf, line_col, ts);
}

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

/* ---- Graphs page ------------------------------------------------------- */

static void draw_graphs(int sw, int sh, uint32_t now_ms) {
    int hint_h    = bs_gfx_text_h(bs_ui_text_scale()) + 4;
    int content_y = bs_ui_content_y();
    int content_h = sh - content_y - hint_h;
    int gap       = 3;
    int heap_h    = (content_h * 55) / 100;
    int frame_h   = content_h - heap_h - gap;

    bs_gfx_clear(g_bs_theme.bg);

    char ubuf[16], hbuf[64];
    fmt_uptime(ubuf, sizeof ubuf, now_ms);
    uint16_t cur = s_heap_kb[(s_head - 1 + TOP_SAMPLES) % TOP_SAMPLES];
    snprintf(hbuf, sizeof hbuf, "Top  |  heap %u kB  |  %s", (unsigned)cur, ubuf);
    bs_ui_draw_header(hbuf);

    draw_graph(0, content_y,              sw, heap_h,  s_heap_kb,  "Heap free", "kB", g_bs_theme.accent);
    draw_graph(0, content_y + heap_h + gap, sw, frame_h, s_frame_ms, "Frame",     "ms", g_bs_theme.primary);

    bs_ui_draw_hint("right:sysinfo  back:exit");
}

/* ---- Sysinfo rows ------------------------------------------------------- */

#define SYSINFO_MAX_ROWS 56

typedef struct {
    char  label[28];
    char  value[64];
    bool  is_section;
    bool  warn;
} sysinfo_row_t;

static sysinfo_row_t s_info_rows[SYSINFO_MAX_ROWS];
static int           s_info_nrows;
static int           s_info_scroll;
static int           s_marquee_tick;   /* increments each sysinfo frame */

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

static void build_sysinfo(void) {
    s_info_nrows = 0;

    bs_hw_info_t hw;
    bs_hw_get_info(&hw);

    /* ---- Board ---- */
    info_sect("BOARD");
    if (hw.board && hw.board[0])
        info_row("Model",    false, "%s",  hw.board);
    if (hw.chip_rev >= 0)
        info_row("Chip rev", false, "%d",  hw.chip_rev);
    if (hw.sdk_ver && hw.sdk_ver[0])
        info_row("SDK",      false, "%s",  hw.sdk_ver);

    /* ---- CPU ---- */
    info_sect("CPU");
    info_row("Frequency", false, "%u MHz", hw.cpu_mhz);
    if (hw.cores > 0)
        info_row("Cores", false, "%d", hw.cores);
    if (hw.task_count > 0)
        info_row("Tasks", false, "%u running", hw.task_count);

    /* ---- Memory ---- */
    info_sect("MEMORY");
    if (hw.heap_total_kb)
        info_row("Heap free / total", false, "%u / %u kB",
                 hw.heap_free_kb, hw.heap_total_kb);
    else
        info_row("Heap free", false, "%u kB", hw.heap_free_kb);
    if (hw.heap_internal_kb && hw.heap_internal_kb != hw.heap_free_kb)
        info_row("Internal free", false, "%u kB", hw.heap_internal_kb);
    info_row("Min ever heap", hw.heap_min_kb < 20, "%u kB", hw.heap_min_kb);
    if (hw.psram_total_kb) {
        info_row("PSRAM free / total", false, "%u / %u kB",
                 hw.psram_free_kb, hw.psram_total_kb);
    }

    /* ---- Flash ---- */
    info_sect("FLASH");
    if (hw.flash_kb)
        info_row("Chip size",   false, "%u kB", hw.flash_kb);
    if (hw.firmware_kb)
        info_row("Sketch size", false, "%u kB", hw.firmware_kb);
    if (hw.flash_free_kb)
        info_row("Free (OTA)",  false, "%u kB", hw.flash_free_kb);

    /* ---- Tasks ---- */
    bs_hw_task_t tasks[BS_HW_MAX_TASKS];
    int ntasks = bs_hw_task_list(tasks, BS_HW_MAX_TASKS);

    if (ntasks > 0) {
        qsort(tasks, (size_t)ntasks, sizeof tasks[0], task_cmp_hwm);
        info_sect("TASKS (by stack headroom)");
        for (int i = 0; i < ntasks; i++) {
            char label[28];
            snprintf(label, sizeof label, "  %s  p%u",
                     tasks[i].name, tasks[i].priority);
            bool warn = (tasks[i].stack_hwm_b < 512);
            info_row(label, warn, "%u B", tasks[i].stack_hwm_b);
        }
    }
}

/* ---- Marquee text renderer --------------------------------------------- */

/*
 * Renders `text` at (x,y) clipped to avail_w pixels.
 * If text is wider than avail_w it scrolls character-by-character
 * based on s_marquee_tick (advances ~3 chars/sec at 30 Hz).
 */
static void render_marquee(int x, int y, const char* text,
                            int avail_w, float ts, bs_color_t col) {
    int tw = bs_gfx_text_w(text, ts);
    if (tw <= avail_w) {
        bs_gfx_text(x, y, text, col, ts);
        return;
    }
    int cw  = bs_gfx_text_w("W", ts); if (cw <= 0) cw = 6;
    int cpr = avail_w / cw;           /* chars visible at once */
    int len = (int)strlen(text);
    if (cpr >= len) { bs_gfx_text(x, y, text, col, ts); return; }

    /* Scroll: 10 frames/char, pause 20 frames at wrap */
    int cycle  = (len - cpr + 1) * 10 + 20;
    int t      = s_marquee_tick % cycle;
    int offset = (t < 20) ? 0 : (t - 20) / 10;
    if (offset + cpr > len) offset = len - cpr;
    if (offset < 0) offset = 0;

    char tmp[72]; if (cpr > 71) cpr = 71;
    memcpy(tmp, text + offset, (size_t)cpr);
    tmp[cpr] = '\0';
    bs_gfx_text(x, y, tmp, col, ts);
}

/* ---- Sysinfo page ------------------------------------------------------- */

static void draw_sysinfo(int sw, int sh, uint32_t now_ms) {
    float ts   = bs_ui_text_scale(); if (ts > 2.0f) ts = 2.0f;
    int lh     = bs_gfx_text_h(ts) + 3;
    int hint_h = bs_gfx_text_h(ts) + 4;

    build_sysinfo();  /* refresh data every frame */

    int content_y = bs_ui_content_y();
    int visible   = (sh - content_y - hint_h) / lh;
    if (visible < 1) visible = 1;

    /* Clamp scroll */
    int max_scroll = s_info_nrows - visible;
    if (max_scroll < 0)  max_scroll = 0;
    if (s_info_scroll > max_scroll) s_info_scroll = max_scroll;
    if (s_info_scroll < 0)          s_info_scroll = 0;

    bs_gfx_clear(g_bs_theme.bg);

    char ubuf[16], hbuf[48];
    fmt_uptime(ubuf, sizeof ubuf, now_ms);
    snprintf(hbuf, sizeof hbuf, "Sysinfo  |  up %s", ubuf);
    bs_ui_draw_header(hbuf);

    /* Value column: right half of screen */
    int lx = 6;
    int vx = sw / 2;
    int avail_v = sw - vx - 4;   /* available width for value text */

    for (int i = 0; i < visible; i++) {
        int ri = i + s_info_scroll;
        if (ri >= s_info_nrows) break;
        const sysinfo_row_t* r = &s_info_rows[ri];

        int row_y = content_y + i * lh;

        if (r->is_section) {
            /* Section header: accent color, indented left */
            bs_gfx_hline(lx, row_y + lh - 2, sw - lx * 2, g_bs_theme.dim);
            bs_gfx_text(lx, row_y, r->label, g_bs_theme.accent, ts);
        } else {
            bs_color_t vc = r->warn ? g_bs_theme.warn : g_bs_theme.primary;
            bs_gfx_text(lx + 8, row_y, r->label, g_bs_theme.dim, ts);
            render_marquee(vx, row_y, r->value, avail_v, ts, vc);
        }
    }

    /* Scroll indicators */
    if (s_info_scroll > 0) {
        int iw = bs_gfx_text_w("^ more", 1);
        bs_gfx_text(sw - iw - 4, content_y + 2, "^ more", g_bs_theme.dim, 1);
    }
    if (s_info_scroll < max_scroll) {
        int iw = bs_gfx_text_w("v more", 1);
        bs_gfx_text(sw - iw - 4, sh - hint_h - bs_gfx_text_h(1) - 2,
                    "v more", g_bs_theme.dim, 1);
    }

    bs_ui_draw_hint("up/dn:scroll  right:graphs  back:exit");
}

/* ---- App --------------------------------------------------------------- */

static void top_run(const bs_arch_t* arch) {
    s_head  = 0;
    s_count = 0;
    memset(s_heap_kb,  0, sizeof s_heap_kb);
    memset(s_frame_ms, 0, sizeof s_frame_ms);

    int sw = bs_gfx_width();
    int sh = bs_gfx_height();
    int page = 0;

    s_info_scroll  = 0;
    s_marquee_tick = 0;

    uint32_t prev_graph_ms   = arch->millis();
    uint32_t prev_marquee_ms = prev_graph_ms;
    bool sysinfo_dirty = false;

    for (;;) {
        uint32_t now_ms = arch->millis();

        /* --- Nav: drain ALL pending events before drawing.
         *     With ISR-captured encoder, multiple detents can queue up during
         *     a 50 ms draw.  Processing them all before the next present means
         *     the displayed position always reflects the latest input rather
         *     than lagging one detent per frame.                              */
        bool quit = false;
        bs_nav_id_t nav;
        while (!quit && (nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_BACK:
                    quit = true;
                    break;

                /* right / select: toggle graphs ↔ sysinfo */
                case BS_NAV_SELECT:
                case BS_NAV_RIGHT:
                    if (page == 0) {
                        page = 1;
                        s_info_scroll  = 0;
                        s_marquee_tick = 0;
                        prev_marquee_ms = now_ms;
                        sysinfo_dirty  = true;
                    } else {
                        page = 0;
                    }
                    break;

                /* up/down (W/S or knob rotate): scroll sysinfo */
                case BS_NAV_UP:
                case BS_NAV_PREV:
                    if (page == 1 && s_info_scroll > 0) {
                        s_info_scroll--;
                        sysinfo_dirty = true;
                    }
                    break;
                case BS_NAV_DOWN:
                case BS_NAV_NEXT:
                    if (page == 1) {
                        s_info_scroll++;   /* clamped in draw_sysinfo */
                        sysinfo_dirty = true;
                    }
                    break;

                default:
                    break;
            }
        }
        if (quit) return;

        if (page == 0) {
            /* Graph page: sample hw + redraw at ~30 fps.
             * Draw blocks ~50 ms but that is fine — only button events
             * (slow edge-detected) are needed on this page.              */
            if (now_ms - prev_graph_ms >= 33) {
                uint32_t fms = now_ms - prev_graph_ms;
                prev_graph_ms = now_ms;
                if (fms > 0xFFFF) fms = 0xFFFF;
                bs_hw_info_t hw;
                bs_hw_get_info(&hw);
                ring_push((uint16_t)hw.heap_free_kb, (uint16_t)fms);
                draw_graphs(sw, sh, now_ms);
                bs_debug_frame();
                bs_gfx_present();
            }
        } else {
            /* Sysinfo page: tick marquee every 100 ms; redraw only when
             * something changed.  Between redraws the loop is near-idle so
             * the encoder is polled at the full ~200 Hz rate.              */
            if (now_ms - prev_marquee_ms >= 100) {
                prev_marquee_ms = now_ms;
                s_marquee_tick++;
                sysinfo_dirty = true;
            }
            if (sysinfo_dirty) {
                sysinfo_dirty = false;
                draw_sysinfo(sw, sh, now_ms);
                bs_debug_frame();
                bs_gfx_present();
            }
        }

    }
}

const bs_app_t app_top = {
    .name   = "Top",
    .icon   = bs_chart_32,
    .icon_w = 32,
    .icon_h = 32,
    .run    = top_run,
};
