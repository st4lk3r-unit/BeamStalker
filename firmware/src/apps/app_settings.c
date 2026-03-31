/*
 * app_settings.c - Settings menu app.
 *
 * Sub-menus:
 *   Display    — Layout, Grid Cols, Grid Rows, Text Scale, Brightness
 *   Appearance — Palette, Border
 *   System     — Voltage display, Firmware info
 */
#include "apps/app_settings.h"
#include "bs/bs_assets.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_menu.h"
#include "bs/bs_gfx.h"
#include "bs/bs_debug.h"
#include "bs/bs_fs.h"
#include "bs/bs_ui.h"
#include "beamstalker.h"

#include <string.h>
#include <stdio.h>

/* ── Lookup tables ───────────────────────────────────────────────────────── */

static const char* const k_layout_names[] = {"Auto","Grid","Slideshow","List"};
#define N_LAYOUTS 4

static const char* const k_grid_names[] = {"Auto", "2 max", "3 max", "4 max"};
static const int         k_grid_vals[]  = {0, 2, 3, 4};
#define N_GRID 4

static const char* const k_grid_rows_names[] = {"Auto", "1 row", "2 rows", "3 rows"};
static const int         k_grid_rows_vals[]  = {0, 1, 2, 3};
#define N_GRID_ROWS 4

static const char* const k_scale_names[] = {"Small","Medium","Large"};
static const int         k_scale_vals[]  = {1, 2, 3};
#define N_SCALES 3

static const char* const k_bright_names[] = {"25%","50%","75%","100%"};
static const int         k_bright_vals[]  = {25, 50, 75, 100};
#define N_BRIGHT 4

static const char* const k_voltage_names[] = {"Off", "On"};
#define N_VOLTAGE 2

/* ── Pages ───────────────────────────────────────────────────────────────── */

typedef enum {
    SETT_MAIN = 0,
    SETT_DISPLAY,
    SETT_APPEARANCE,
    SETT_SYSTEM
} sett_page_t;

#define N_MAIN         3
#define N_DISPLAY      5
#define N_APPEARANCE   2
#define N_SYSTEM       1   /* voltage only; firmware row is info */

/* ── State ───────────────────────────────────────────────────────────────── */

static sett_page_t s_page;
static int         s_cursor[4];  /* per-page cursor: [MAIN, DISPLAY, APPEARANCE, SYSTEM] */

static int s_layout_idx;
static int s_grid_cols_idx;
static int s_grid_rows_idx;
static int s_palette_idx;
static int s_border_idx;
static int s_text_scale_idx;
static int s_bright_idx;
static int s_voltage_idx;

/* ── Persist ─────────────────────────────────────────────────────────────── */

static void settings_save(void) {
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "layout=%d\ngrid_cols=%d\ngrid_rows=%d\npalette=%d\nborder=%d\n"
        "text_scale=%d\nbrightness=%d\nshow_voltage=%d\n",
        s_layout_idx, k_grid_vals[s_grid_cols_idx],
        k_grid_rows_vals[s_grid_rows_idx],
        s_palette_idx, s_border_idx,
        k_scale_vals[s_text_scale_idx],
        k_bright_vals[s_bright_idx], s_voltage_idx);
    bs_fs_write_file("settings.cfg", buf, (size_t)n);
}

static void settings_load(void) {
    char buf[192]; size_t n = 0;
    if (bs_fs_read_file("settings.cfg", buf, sizeof buf - 1, &n) < 0) return;
    buf[n] = '\0';
    char* line = buf;
    while (*line) {
        char* nl = line; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = '\0';
        int val = 0;
        if (sscanf(line, "layout=%d",      &val) == 1) s_layout_idx  = val;
        if (sscanf(line, "grid_cols=%d",   &val) == 1) {
            for (int i = 0; i < N_GRID; i++)
                if (k_grid_vals[i] == val) { s_grid_cols_idx = i; break; }
        }
        if (sscanf(line, "grid_rows=%d",   &val) == 1) {
            for (int i = 0; i < N_GRID_ROWS; i++)
                if (k_grid_rows_vals[i] == val) { s_grid_rows_idx = i; break; }
        }
        if (sscanf(line, "palette=%d",     &val) == 1) s_palette_idx = val;
        if (sscanf(line, "theme=%d",       &val) == 1) s_palette_idx = val;   /* compat */
        if (sscanf(line, "border=%d",      &val) == 1) s_border_idx  = val;
        if (sscanf(line, "text_scale=%d",  &val) == 1) {
            for (int i = 0; i < N_SCALES; i++)
                if (k_scale_vals[i] == val) { s_text_scale_idx = i; break; }
        }
        if (sscanf(line, "brightness=%d",  &val) == 1) {
            for (int i = 0; i < N_BRIGHT; i++)
                if (k_bright_vals[i] == val) { s_bright_idx = i; break; }
        }
        if (sscanf(line, "show_voltage=%d",&val) == 1)
            s_voltage_idx = (val != 0) ? 1 : 0;
        *nl = save;
        line = nl + (*nl ? 1 : 0);
    }
    if (s_palette_idx < 0 || s_palette_idx >= BS_PALETTE_COUNT)      s_palette_idx   = 0;
    if (s_border_idx  < 0 || s_border_idx  >= BS_BORDER_STYLE_COUNT) s_border_idx    = 0;
    if (s_grid_cols_idx < 0 || s_grid_cols_idx >= N_GRID)            s_grid_cols_idx = 0;
    if (s_grid_rows_idx < 0 || s_grid_rows_idx >= N_GRID_ROWS)       s_grid_rows_idx = 0;
}

/* ── Apply helpers ───────────────────────────────────────────────────────── */

static void apply_layout(void)     { bs_menu_set_mode((bs_menu_mode_t)s_layout_idx); bs_menu_invalidate(); }
static void apply_grid_cols(void)  { bs_ui_set_grid_max_cols(k_grid_vals[s_grid_cols_idx]); bs_menu_invalidate(); }
static void apply_grid_rows(void)  { bs_ui_set_grid_max_rows(k_grid_rows_vals[s_grid_rows_idx]); bs_menu_invalidate(); }
static void apply_appearance(void) { bs_theme_apply(s_palette_idx, (bs_border_style_t)s_border_idx); bs_menu_invalidate(); }
static void apply_text_scale(void) { bs_ui_set_text_scale(k_scale_vals[s_text_scale_idx]); bs_menu_invalidate(); }
static void apply_brightness(void) { bs_ui_set_brightness(k_bright_vals[s_bright_idx]); }
static void apply_voltage(void)    { bs_ui_set_show_voltage(s_voltage_idx != 0); }

/* ── Shared row draw ─────────────────────────────────────────────────────── */

#define COL_LABEL_X      8
#define ROW_H_FOR(ts)       (bs_gfx_text_h(ts) + 8)
#define COL_VALUE_X_FOR(ts) (COL_LABEL_X + bs_gfx_text_w("Brightness", (ts)) + 12)

static void draw_row(int row_idx, const char* label, const char* value,
                     bool selected, int ts) {
    int sw    = bs_gfx_width();
    int rh    = ROW_H_FOR(ts);
    int cvx   = COL_VALUE_X_FOR(ts);
    int y     = bs_ui_header_h() + 4 + row_idx * rh;
    int pad_y = (rh - bs_gfx_text_h(ts)) / 2;

    if (selected) bs_gfx_fill_rect(0, y - 1, sw, rh, g_bs_theme.dim);
    bs_gfx_text(COL_LABEL_X, y + pad_y, label, g_bs_theme.secondary, ts);
    bs_color_t vcol = selected ? g_bs_theme.accent : g_bs_theme.primary;
    int aw = bs_gfx_text_w("<", ts);
    bs_gfx_text(cvx,      y + pad_y, "<",   g_bs_theme.dim, ts);
    int vx = cvx + aw + 4;
    bs_gfx_text(vx,       y + pad_y, value, vcol,           ts);
    int nx = vx + bs_gfx_text_w(value, ts) + 4;
    bs_gfx_text(nx,       y + pad_y, ">",   g_bs_theme.dim, ts);
}

static void draw_info_row(int row_idx, const char* label, const char* value, int ts) {
    int rh    = ROW_H_FOR(ts);
    int cvx   = COL_VALUE_X_FOR(ts);
    int y     = bs_ui_header_h() + 4 + row_idx * rh;
    int pad_y = (rh - bs_gfx_text_h(ts)) / 2;

    bs_gfx_text(COL_LABEL_X, y + pad_y, label, g_bs_theme.dim,       ts);
    bs_gfx_text(cvx,         y + pad_y, value, g_bs_theme.secondary, ts);
}

/* ── Draw: main menu ─────────────────────────────────────────────────────── */

static void draw_main(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 10;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings");

    typedef struct { const char* name; const char* desc; } entry_t;
    static const entry_t k_entries[N_MAIN] = {
        { "Display",    "Layout, grid, scale, brightness" },
        { "Appearance", "Palette and border style"        },
        { "System",     "Voltage display, firmware info"  },
    };

    for (int i = 0; i < N_MAIN; i++) {
        bool sel = (i == s_cursor[SETT_MAIN]);
        int  y   = cy + i * lh;
        if (sel) bs_gfx_fill_rect(0, y - 3, sw, lh - 1, g_bs_theme.dim);
        bs_color_t nc = sel ? g_bs_theme.accent  : g_bs_theme.primary;
        bs_color_t dc = sel ? g_bs_theme.primary : g_bs_theme.dim;
        bs_gfx_text(8, y,                       k_entries[i].name, nc, ts);
        bs_gfx_text(8, y + bs_gfx_text_h(ts) + 2,
                    k_entries[i].desc, dc, ts2);
    }

    bs_ui_draw_hint("SELECT=open  BACK=exit");
    bs_gfx_present();
}

/* ── Draw: Display sub-menu ──────────────────────────────────────────────── */

static void draw_display(void) {
    int ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / Display");

    int c = s_cursor[SETT_DISPLAY];
    draw_row(0, "Layout",     k_layout_names[s_layout_idx],       c == 0, ts);
    draw_row(1, "Grid Cols",  k_grid_names[s_grid_cols_idx],      c == 1, ts);
    draw_row(2, "Grid Rows",  k_grid_rows_names[s_grid_rows_idx], c == 2, ts);
    draw_row(3, "Text Scale", k_scale_names[s_text_scale_idx],    c == 3, ts);
    draw_row(4, "Brightness", k_bright_names[s_bright_idx],       c == 4, ts);

    bs_ui_draw_hint("<<>>:change  up/dn:move  BACK=back");
    bs_gfx_present();
}

/* ── Draw: Appearance sub-menu ───────────────────────────────────────────── */

static void draw_appearance(void) {
    int ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / Appearance");

    int c = s_cursor[SETT_APPEARANCE];
    draw_row(0, "Palette", bs_palette_names[s_palette_idx],     c == 0, ts);
    draw_row(1, "Border",  bs_border_style_names[s_border_idx], c == 1, ts);

    bs_ui_draw_hint("<<>>:change  up/dn:move  BACK=back");
    bs_gfx_present();
}

/* ── Draw: System sub-menu ───────────────────────────────────────────────── */

static void draw_system(void) {
    int ts = bs_ui_text_scale();
    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / System");

    int c = s_cursor[SETT_SYSTEM];
    draw_row( 0, "Voltage",  k_voltage_names[s_voltage_idx], c == 0, ts);
    draw_info_row(1, "Firmware", "BeamStalker v" BS_VERSION,         ts);

    bs_ui_draw_hint("<<>>:change  BACK=back");
    bs_gfx_present();
}

/* ── Run ─────────────────────────────────────────────────────────────────── */

#define CYCLE_LEFT(idx, n)  (idx) = ((idx) + (n) - 1) % (n)
#define CYCLE_RIGHT(idx, n) (idx) = ((idx) + 1) % (n)

static void settings_run(const bs_arch_t* arch) {
    (void)arch;

    s_page = SETT_MAIN;
    memset(s_cursor, 0, sizeof s_cursor);

    s_layout_idx     = (int)bs_menu_get_mode();
    s_grid_cols_idx  = 0;
    s_grid_rows_idx  = 0;
    s_palette_idx    = 0;
    s_border_idx     = (int)g_bs_theme.border;
    s_text_scale_idx = 0;
    s_bright_idx     = 3;
    s_voltage_idx    = bs_ui_show_voltage() ? 1 : 0;

    { int cur = bs_ui_grid_max_cols();
      for (int i = 0; i < N_GRID; i++)
          if (k_grid_vals[i] == cur) { s_grid_cols_idx = i; break; } }

    { int cur = bs_ui_grid_max_rows();
      for (int i = 0; i < N_GRID_ROWS; i++)
          if (k_grid_rows_vals[i] == cur) { s_grid_rows_idx = i; break; } }

    for (int i = 0; i < BS_PALETTE_COUNT; i++) {
        const bs_palette_t* p = bs_palettes[i];
        if (p->primary.r == g_bs_theme.primary.r &&
            p->primary.g == g_bs_theme.primary.g &&
            p->primary.b == g_bs_theme.primary.b) {
            s_palette_idx = i; break;
        }
    }

    { int cur = bs_ui_text_scale();
      for (int i = 0; i < N_SCALES; i++)
          if (k_scale_vals[i] == cur) { s_text_scale_idx = i; break; } }

    { int cur = bs_ui_brightness();
      for (int i = 0; i < N_BRIGHT; i++)
          if (k_bright_vals[i] == cur) { s_bright_idx = i; break; } }

    settings_load();

    bool dirty = true;
    while (true) {
        if (dirty) {
            switch (s_page) {
                case SETT_MAIN:       draw_main();       break;
                case SETT_DISPLAY:    draw_display();    break;
                case SETT_APPEARANCE: draw_appearance(); break;
                case SETT_SYSTEM:     draw_system();     break;
            }
            bs_debug_frame();
            bs_gfx_present();
            dirty = false;
        }

        bs_nav_id_t nav = bs_nav_poll();
        if (nav == BS_NAV_NONE) continue;

        if (s_page == SETT_MAIN) {
            switch (nav) {
                case BS_NAV_BACK: return;
                case BS_NAV_UP:   case BS_NAV_PREV:
                    s_cursor[SETT_MAIN] = (s_cursor[SETT_MAIN] + N_MAIN - 1) % N_MAIN;
                    dirty = true; break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    s_cursor[SETT_MAIN] = (s_cursor[SETT_MAIN] + 1) % N_MAIN;
                    dirty = true; break;
                case BS_NAV_SELECT:
                    s_page = (sett_page_t)(s_cursor[SETT_MAIN] + 1);
                    dirty = true; break;
                default: break;
            }
            continue;
        }

        /* ── Sub-menu navigation ── */
        int  n   = 0;
        int* cur = &s_cursor[s_page];
        switch (s_page) {
            case SETT_DISPLAY:    n = N_DISPLAY;    break;
            case SETT_APPEARANCE: n = N_APPEARANCE; break;
            case SETT_SYSTEM:     n = N_SYSTEM;     break;
            default: break;
        }

        switch (nav) {
            case BS_NAV_BACK:
                s_page = SETT_MAIN;
                dirty  = true;
                break;
            case BS_NAV_UP:   case BS_NAV_PREV:
                *cur = (*cur + n - 1) % n;
                dirty = true; break;
            case BS_NAV_DOWN: case BS_NAV_NEXT:
                *cur = (*cur + 1) % n;
                dirty = true; break;
            case BS_NAV_LEFT:
                if (s_page == SETT_DISPLAY) {
                    switch (*cur) {
                        case 0: CYCLE_LEFT(s_layout_idx,     N_LAYOUTS);            apply_layout();     break;
                        case 1: CYCLE_LEFT(s_grid_cols_idx,  N_GRID);               apply_grid_cols();  break;
                        case 2: CYCLE_LEFT(s_grid_rows_idx,  N_GRID_ROWS);          apply_grid_rows();  break;
                        case 3: CYCLE_LEFT(s_text_scale_idx, N_SCALES);             apply_text_scale(); break;
                        case 4: CYCLE_LEFT(s_bright_idx,     N_BRIGHT);             apply_brightness(); break;
                    }
                } else if (s_page == SETT_APPEARANCE) {
                    switch (*cur) {
                        case 0: CYCLE_LEFT(s_palette_idx,  BS_PALETTE_COUNT);      apply_appearance(); break;
                        case 1: CYCLE_LEFT(s_border_idx,   BS_BORDER_STYLE_COUNT); apply_appearance(); break;
                    }
                } else if (s_page == SETT_SYSTEM) {
                    if (*cur == 0) { CYCLE_LEFT(s_voltage_idx, N_VOLTAGE); apply_voltage(); }
                }
                settings_save(); dirty = true; break;
            case BS_NAV_RIGHT:
            case BS_NAV_SELECT:
                if (s_page == SETT_DISPLAY) {
                    switch (*cur) {
                        case 0: CYCLE_RIGHT(s_layout_idx,     N_LAYOUTS);            apply_layout();     break;
                        case 1: CYCLE_RIGHT(s_grid_cols_idx,  N_GRID);               apply_grid_cols();  break;
                        case 2: CYCLE_RIGHT(s_grid_rows_idx,  N_GRID_ROWS);          apply_grid_rows();  break;
                        case 3: CYCLE_RIGHT(s_text_scale_idx, N_SCALES);             apply_text_scale(); break;
                        case 4: CYCLE_RIGHT(s_bright_idx,     N_BRIGHT);             apply_brightness(); break;
                    }
                } else if (s_page == SETT_APPEARANCE) {
                    switch (*cur) {
                        case 0: CYCLE_RIGHT(s_palette_idx,  BS_PALETTE_COUNT);      apply_appearance(); break;
                        case 1: CYCLE_RIGHT(s_border_idx,   BS_BORDER_STYLE_COUNT); apply_appearance(); break;
                    }
                } else if (s_page == SETT_SYSTEM) {
                    if (*cur == 0) { CYCLE_RIGHT(s_voltage_idx, N_VOLTAGE); apply_voltage(); }
                }
                settings_save(); dirty = true; break;
            default: break;
        }
    }
}

#undef CYCLE_LEFT
#undef CYCLE_RIGHT

/* ── App descriptor ──────────────────────────────────────────────────────── */

const bs_app_t app_settings = {
    .name   = "Settings",
    .icon   = bs_gear_32,
    .icon_w = 32,
    .icon_h = 32,
    .run    = settings_run,
};
