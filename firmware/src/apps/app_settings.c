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
#include "bs/bs_log.h"
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

static const char* const k_scale_names[] = {"Small","Mid-S","Medium","Mid-L","Large"};
static const float       k_scale_vals[]  = {1.0f, 1.5f, 2.0f, 2.5f, 3.0f};
#define N_SCALES 5

static const char* const k_bright_names[] = {"25%","50%","75%","100%"};
static const int         k_bright_vals[]  = {25, 50, 75, 100};
#define N_BRIGHT 4

static const char* const k_voltage_names[] = {"Off", "On"};
#define N_VOLTAGE 2

static const char* const k_carousel_names[] = {"Off", "On"};
#define N_CAROUSEL 2

static const char* const k_header_brand_names[] = {"Text", "Logo"};
#define N_HEADER_BRAND 2

/* ── Pages ───────────────────────────────────────────────────────────────── */

typedef enum {
    SETT_MAIN = 0,
    SETT_DISPLAY,
    SETT_APPEARANCE,
    SETT_SYSTEM
} sett_page_t;

#define N_MAIN         3
#define N_DISPLAY      7
#define N_APPEARANCE   2
#define N_SYSTEM       4   /* Voltage, SD info, Format SD, Firmware */

/* ── State ───────────────────────────────────────────────────────────────── */

static sett_page_t s_page;
static int         s_cursor[4];  /* per-page cursor: [MAIN, DISPLAY, APPEARANCE, SYSTEM] */
static int         s_scroll[4];  /* per-page scroll offset */
static bool        s_format_confirm = false;

static int s_layout_idx;
static int s_grid_cols_idx;
static int s_grid_rows_idx;
static int s_palette_idx;
static int s_border_idx;
static int s_text_scale_idx;
static int s_bright_idx;
static int s_voltage_idx;
static int s_carousel_idx;
static int s_header_brand_idx;

/* ── Persist ─────────────────────────────────────────────────────────────── */

static void settings_save(void) {
    char buf[256];
    int n = snprintf(buf, sizeof buf,
        "layout=%d\ngrid_cols=%d\ngrid_rows=%d\npalette=%d\nborder=%d\n"
        "text_scale=%.1f\nbrightness=%d\nshow_voltage=%d\ncarousel=%d\nheader_brand=%d\n",
        s_layout_idx, k_grid_vals[s_grid_cols_idx],
        k_grid_rows_vals[s_grid_rows_idx],
        s_palette_idx, s_border_idx,
        (double)k_scale_vals[s_text_scale_idx],
        k_bright_vals[s_bright_idx], s_voltage_idx, s_carousel_idx, s_header_brand_idx);
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
        { float fv = 0.0f;
          if (sscanf(line, "text_scale=%f", &fv) == 1) {
            for (int i = 0; i < N_SCALES; i++)
                if (k_scale_vals[i] == fv) { s_text_scale_idx = i; break; }
          }
        }
        if (sscanf(line, "brightness=%d",  &val) == 1) {
            for (int i = 0; i < N_BRIGHT; i++)
                if (k_bright_vals[i] == val) { s_bright_idx = i; break; }
        }
        if (sscanf(line, "show_voltage=%d",&val) == 1)
            s_voltage_idx = (val != 0) ? 1 : 0;
        if (sscanf(line, "header_brand=%d",&val) == 1)
            s_header_brand_idx = (val != 0) ? 1 : 0;
        *nl = save;
        line = nl + (*nl ? 1 : 0);
    }
    if (s_palette_idx < 0 || s_palette_idx >= BS_PALETTE_COUNT)      s_palette_idx   = 0;
    if (s_border_idx  < 0 || s_border_idx  >= BS_BORDER_STYLE_COUNT) s_border_idx    = 0;
    if (s_grid_cols_idx < 0 || s_grid_cols_idx >= N_GRID)            s_grid_cols_idx = 0;
    if (s_grid_rows_idx < 0 || s_grid_rows_idx >= N_GRID_ROWS)       s_grid_rows_idx = 0;
    if (s_header_brand_idx < 0 || s_header_brand_idx >= N_HEADER_BRAND) s_header_brand_idx = 0;
}

/* ── Apply helpers ───────────────────────────────────────────────────────── */

static void apply_layout(void)     { bs_menu_set_mode((bs_menu_mode_t)s_layout_idx); bs_menu_invalidate(); }
static void apply_grid_cols(void)  { bs_ui_set_grid_max_cols(k_grid_vals[s_grid_cols_idx]); bs_menu_invalidate(); }
static void apply_grid_rows(void)  { bs_ui_set_grid_max_rows(k_grid_rows_vals[s_grid_rows_idx]); bs_menu_invalidate(); }
static void apply_appearance(void) { bs_theme_apply(s_palette_idx, (bs_border_style_t)s_border_idx); bs_menu_invalidate(); }
static void apply_text_scale(void) { bs_ui_set_text_scale((float)k_scale_vals[s_text_scale_idx]); bs_menu_invalidate(); }
static void apply_brightness(void) { bs_ui_set_brightness(k_bright_vals[s_bright_idx]); }
static void apply_voltage(void)    { bs_ui_set_show_voltage(s_voltage_idx != 0); }
static void apply_carousel(void)   { bs_ui_set_carousel(s_carousel_idx != 0); }
static void apply_header_brand(void){ bs_ui_set_header_brand_mode(s_header_brand_idx != 0); bs_menu_invalidate(); }

/* ── Shared row draw ─────────────────────────────────────────────────────── */

#define COL_LABEL_X      8
#define ROW_H_FOR(ts)       (bs_gfx_text_h(ts) + 8)
#define COL_VALUE_X_FOR(ts) (COL_LABEL_X + bs_gfx_text_w("Brightness", (ts)) + 12)

/* How many sub-menu rows (with 8px padding) fit in the content area */
static int sett_visible_rows(float ts) {
    int vis = bs_ui_content_h() / ROW_H_FOR(ts);
    return vis < 1 ? 1 : vis;
}

/* Adjust scroll so cursor stays visible */
static void sett_clamp_scroll(sett_page_t page, float ts) {
    int vis = sett_visible_rows(ts);
    bs_ui_list_clamp_scroll(s_cursor[page], &s_scroll[page], 64 /* generous */, vis);
}

/*
 * Draw a cycling-value row (< value >) at screen row row_idx.
 * '>' is pinned at the right edge; value is carousel-clipped in between.
 */
static void draw_row(int row_idx, const char* label, const char* value,
                     bool selected, float ts) {
    int sw    = bs_gfx_width();
    int rh    = ROW_H_FOR(ts);
    int cvx   = COL_VALUE_X_FOR(ts);
    int y     = bs_ui_content_y() + 4 + row_idx * rh;
    int pad_y = (rh - bs_gfx_text_h(ts)) / 2;

    if (selected) bs_gfx_fill_rect(0, y - 1, sw, rh, g_bs_theme.dim);

    int aw  = bs_gfx_text_w("<", ts);
    int lw  = cvx - COL_LABEL_X - 4;          /* label column width */
    int rx  = sw - aw - 4;                     /* x of pinned '>' */
    int vx  = cvx + aw + 4;                    /* x where value starts */
    int vw  = rx - 4 - vx;                     /* value column width */

    if (lw > 0)
        bs_ui_draw_text_box(COL_LABEL_X, y + pad_y, lw, label, g_bs_theme.secondary, ts, selected);

    bs_color_t vcol = selected ? g_bs_theme.accent : g_bs_theme.primary;
    bs_gfx_text(cvx, y + pad_y, "<", g_bs_theme.dim, ts);
    if (vw > 0)
        bs_ui_draw_text_box(vx, y + pad_y, vw, value, vcol, ts, selected);
    bs_gfx_text(rx,  y + pad_y, ">", g_bs_theme.dim, ts);
}

/* Draw a read-only info row; carousels when selected. Uses bs_ui_draw_kv_row. */
static void draw_info_row(int row_idx, const char* label, const char* value,
                          bool selected, float ts) {
    int y = bs_ui_content_y() + 4 + row_idx * ROW_H_FOR(ts);
    bs_ui_draw_kv_row(y, label, value, selected, false, ts);
}

/* Draw an action row (selectable, no value column — just label + '>' arrow). */
static void draw_action_row(int row_idx, const char* label, bool selected, float ts) {
    int sw    = bs_gfx_width();
    int rh    = ROW_H_FOR(ts);
    int y     = bs_ui_content_y() + 4 + row_idx * rh;
    int pad_y = (rh - bs_gfx_text_h(ts)) / 2;

    if (selected) bs_gfx_fill_rect(0, y - 1, sw, rh, g_bs_theme.dim);
    bs_color_t lc = selected ? g_bs_theme.accent : g_bs_theme.primary;
    int aw = bs_gfx_text_w(">", ts);
    int lw = sw - aw - 8 - COL_LABEL_X;
    if (lw > 0)
        bs_ui_draw_text_box(COL_LABEL_X, y + pad_y, lw, label, lc, ts, selected);
    bs_gfx_text(sw - aw - 4, y + pad_y, ">", g_bs_theme.dim, ts);
}

/* ── Draw: main menu ─────────────────────────────────────────────────────── */

static void draw_main(void) {
    float ts = bs_ui_text_scale();
    int   cy = bs_ui_content_y();
    int   lh = bs_ui_menu_row_h(ts);
    int   vis = bs_ui_menu_visible(ts);

    bs_ui_list_clamp_scroll(s_cursor[SETT_MAIN], &s_scroll[SETT_MAIN], N_MAIN, vis);
    int sc = s_scroll[SETT_MAIN];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings");

    typedef struct { const char* name; const char* desc; } entry_t;
    static const entry_t k_entries[N_MAIN] = {
        { "Display",    "Layout, grid, scale, brightness" },
        { "Appearance", "Palette and border style"        },
        { "System",     "Voltage display, firmware info"  },
    };

    for (int i = sc; i < N_MAIN && (i - sc) < vis; i++)
        bs_ui_draw_menu_row(cy + (i - sc) * lh, k_entries[i].name, k_entries[i].desc,
                            i == s_cursor[SETT_MAIN], ts);

    bs_ui_draw_scroll_arrows(sc, N_MAIN, vis);
    bs_ui_draw_hint("SELECT=open  BACK=exit");
    bs_gfx_present();
}

/* ── Draw: Display sub-menu ──────────────────────────────────────────────── */

static void draw_display(void) {
    float ts  = bs_ui_text_scale();
    int   vis = sett_visible_rows(ts);
    int   sc  = s_scroll[SETT_DISPLAY];
    int   c   = s_cursor[SETT_DISPLAY];

    typedef struct { const char* lbl; const char* val; } row_t;
    const row_t rows[N_DISPLAY] = {
        { "Layout",     k_layout_names[s_layout_idx]       },
        { "Grid Cols",  k_grid_names[s_grid_cols_idx]      },
        { "Grid Rows",  k_grid_rows_names[s_grid_rows_idx] },
        { "Text Scale", k_scale_names[s_text_scale_idx]    },
        { "Brightness", k_bright_names[s_bright_idx]       },
        { "Carousel",   k_carousel_names[s_carousel_idx]   },
        { "Header",     k_header_brand_names[s_header_brand_idx] },
    };

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / Display");
    for (int i = sc; i < sc + vis && i < N_DISPLAY; i++)
        draw_row(i - sc, rows[i].lbl, rows[i].val, c == i, ts);
    bs_ui_draw_scroll_arrows(sc, N_DISPLAY, vis);
    bs_ui_draw_hint("<<>>:change  up/dn:move  BACK=back");
    bs_gfx_present();
}

/* ── Draw: Appearance sub-menu ───────────────────────────────────────────── */

static void draw_appearance(void) {
    float ts  = bs_ui_text_scale();
    int   vis = sett_visible_rows(ts);
    int   sc  = s_scroll[SETT_APPEARANCE];
    int   c   = s_cursor[SETT_APPEARANCE];

    typedef struct { const char* lbl; const char* val; } row_t;
    const row_t rows[N_APPEARANCE] = {
        { "Palette", bs_palette_names[s_palette_idx]     },
        { "Border",  bs_border_style_names[s_border_idx] },
    };

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / Appearance");
    for (int i = sc; i < sc + vis && i < N_APPEARANCE; i++)
        draw_row(i - sc, rows[i].lbl, rows[i].val, c == i, ts);
    bs_ui_draw_scroll_arrows(sc, N_APPEARANCE, vis);
    bs_ui_draw_hint("<<>>:change  up/dn:move  BACK=back");
    bs_gfx_present();
}

/* ── Draw: System sub-menu ───────────────────────────────────────────────── */

static void draw_system(void) {
    float ts  = bs_ui_text_scale();
    int   vis = sett_visible_rows(ts);
    int   sc  = s_scroll[SETT_SYSTEM];
    int   c   = s_cursor[SETT_SYSTEM];

    /* SD status string (row 1) */
    char sd_buf[40];
    if (bs_fs_available())
        snprintf(sd_buf, sizeof sd_buf, "Mounted");
    else {
        const char* e = bs_fs_init_error();
        snprintf(sd_buf, sizeof sd_buf, "%s", e ? e : "Not mounted");
    }

    /* Format SD label (row 2): changes when confirmation pending */
    const char* fmt_label = s_format_confirm ? "Confirm format?" : "Format SD";

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Settings / System");

    for (int i = sc; i < sc + vis && i < N_SYSTEM; i++) {
        int sr = i - sc;  /* screen row */
        switch (i) {
            case 0: draw_row(sr,        "Voltage",   k_voltage_names[s_voltage_idx], c == 0, ts); break;
            case 1: draw_info_row(sr,   "SD Card",   sd_buf,                         c == 1, ts); break;
            case 2: draw_action_row(sr, fmt_label,                                   c == 2, ts); break;
            case 3: draw_info_row(sr,   "Firmware",  BS_FW_NAME " v" BS_VERSION,     c == 3, ts); break;
        }
    }
    bs_ui_draw_scroll_arrows(sc, N_SYSTEM, vis);
    bs_ui_draw_hint(s_format_confirm ? "SELECT=confirm  BACK=cancel" : "SELECT=action  BACK=back");
    bs_gfx_present();
}

/* ── Run ─────────────────────────────────────────────────────────────────── */

#define CYCLE_LEFT(idx, n)  (idx) = ((idx) + (n) - 1) % (n)
#define CYCLE_RIGHT(idx, n) (idx) = ((idx) + 1) % (n)

static void settings_run(const bs_arch_t* arch) {

    s_page = SETT_MAIN;
    memset(s_cursor, 0, sizeof s_cursor);
    memset(s_scroll, 0, sizeof s_scroll);
    s_format_confirm = false;

    s_layout_idx     = (int)bs_menu_get_mode();
    s_grid_cols_idx  = 0;
    s_grid_rows_idx  = 0;
    s_palette_idx    = 0;
    s_border_idx     = (int)g_bs_theme.border;
    s_text_scale_idx = 0;
    s_bright_idx     = 3;
    s_voltage_idx    = bs_ui_show_voltage() ? 1 : 0;
    s_carousel_idx   = bs_ui_carousel_enabled() ? 1 : 0;
    s_header_brand_idx = bs_ui_header_brand_mode() ? 1 : 0;

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

    { float cur = bs_ui_text_scale();
      for (int i = 0; i < N_SCALES; i++)
          if (k_scale_vals[i] == cur) { s_text_scale_idx = i; break; } }

    { int cur = bs_ui_brightness();
      for (int i = 0; i < N_BRIGHT; i++)
          if (k_bright_vals[i] == cur) { s_bright_idx = i; break; } }

    settings_load();

    uint32_t prev_ms = arch->millis();
    uint32_t last_anim_ms = prev_ms;
    bool dirty = true;
    while (true) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        bool anim_due = bs_ui_carousel_enabled() && (uint32_t)(now - last_anim_ms) >= 100U;
        if (dirty || anim_due) {
            switch (s_page) {
                case SETT_MAIN:       draw_main();       break;
                case SETT_DISPLAY:    draw_display();    break;
                case SETT_APPEARANCE: draw_appearance(); break;
                case SETT_SYSTEM:     draw_system();     break;
            }
            bs_debug_frame();
            bs_gfx_present();
            dirty = false;
            if (anim_due) last_anim_ms = now;
        }

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            if (s_page == SETT_MAIN) {
                switch (nav) {
                    case BS_NAV_BACK: return;
                    case BS_NAV_UP:   case BS_NAV_PREV:
                        s_cursor[SETT_MAIN] = (s_cursor[SETT_MAIN] + N_MAIN - 1) % N_MAIN; dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        s_cursor[SETT_MAIN] = (s_cursor[SETT_MAIN] + 1) % N_MAIN; dirty = true; break;
                    case BS_NAV_SELECT:
                        s_page = (sett_page_t)(s_cursor[SETT_MAIN] + 1); dirty = true; break;
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
                    if (s_format_confirm) {
                        s_format_confirm = false;
                    } else {
                        s_page = SETT_MAIN;
                    }
                    dirty = true;
                    break;
                case BS_NAV_UP:   case BS_NAV_PREV:
                    *cur = (*cur + n - 1) % n;
                    s_format_confirm = false;
                    sett_clamp_scroll(s_page, bs_ui_text_scale());
                    dirty = true;
                    break;
                case BS_NAV_DOWN: case BS_NAV_NEXT:
                    *cur = (*cur + 1) % n;
                    s_format_confirm = false;
                    sett_clamp_scroll(s_page, bs_ui_text_scale());
                    dirty = true;
                    break;
                case BS_NAV_LEFT:
                    if (s_page == SETT_DISPLAY) {
                        switch (*cur) {
                            case 0: CYCLE_LEFT(s_layout_idx,     N_LAYOUTS);   apply_layout();     break;
                            case 1: CYCLE_LEFT(s_grid_cols_idx,  N_GRID);      apply_grid_cols();  break;
                            case 2: CYCLE_LEFT(s_grid_rows_idx,  N_GRID_ROWS); apply_grid_rows();  break;
                            case 3: CYCLE_LEFT(s_text_scale_idx, N_SCALES);    apply_text_scale(); break;
                            case 4: CYCLE_LEFT(s_bright_idx,     N_BRIGHT);    apply_brightness(); break;
                            case 5: CYCLE_LEFT(s_carousel_idx,   N_CAROUSEL);    apply_carousel();     break;
                            case 6: CYCLE_LEFT(s_header_brand_idx,N_HEADER_BRAND); apply_header_brand(); break;
                        }
                        settings_save();
                    } else if (s_page == SETT_APPEARANCE) {
                        switch (*cur) {
                            case 0: CYCLE_LEFT(s_palette_idx, BS_PALETTE_COUNT);      apply_appearance(); break;
                            case 1: CYCLE_LEFT(s_border_idx,  BS_BORDER_STYLE_COUNT); apply_appearance(); break;
                        }
                        settings_save();
                    } else if (s_page == SETT_SYSTEM) {
                        if (*cur == 0) { CYCLE_LEFT(s_voltage_idx, N_VOLTAGE); apply_voltage(); settings_save(); }
                    }
                    dirty = true;
                    break;
                case BS_NAV_RIGHT:
                case BS_NAV_SELECT:
                    if (s_page == SETT_DISPLAY) {
                        switch (*cur) {
                            case 0: CYCLE_RIGHT(s_layout_idx,     N_LAYOUTS);   apply_layout();     break;
                            case 1: CYCLE_RIGHT(s_grid_cols_idx,  N_GRID);      apply_grid_cols();  break;
                            case 2: CYCLE_RIGHT(s_grid_rows_idx,  N_GRID_ROWS); apply_grid_rows();  break;
                            case 3: CYCLE_RIGHT(s_text_scale_idx, N_SCALES);    apply_text_scale(); break;
                            case 4: CYCLE_RIGHT(s_bright_idx,     N_BRIGHT);    apply_brightness(); break;
                            case 5: CYCLE_RIGHT(s_carousel_idx,   N_CAROUSEL);    apply_carousel();     break;
                            case 6: CYCLE_RIGHT(s_header_brand_idx,N_HEADER_BRAND); apply_header_brand(); break;
                        }
                        settings_save();
                    } else if (s_page == SETT_APPEARANCE) {
                        switch (*cur) {
                            case 0: CYCLE_RIGHT(s_palette_idx, BS_PALETTE_COUNT);      apply_appearance(); break;
                            case 1: CYCLE_RIGHT(s_border_idx,  BS_BORDER_STYLE_COUNT); apply_appearance(); break;
                        }
                        settings_save();
                    } else if (s_page == SETT_SYSTEM) {
                        switch (*cur) {
                            case 0:
                                CYCLE_RIGHT(s_voltage_idx, N_VOLTAGE);
                                apply_voltage();
                                settings_save();
                                break;
                            case 2: /* Format SD */
                                if (!s_format_confirm) {
                                    s_format_confirm = true;
                                } else {
                                    s_format_confirm = false;
                                    bs_fs_format();
                                    bs_log_flush_sd();
                                }
                                break;
                            default: break;
                        }
                    }
                    dirty = true;
                    break;
                default: break;
            }
        }
#if defined(VARIANT_TPAGER) || defined(VARIANT_TDONGLE_S3) || defined(VARIANT_HELTEC_V3)
        arch->delay_ms(1);
#else
        arch->delay_ms(2);
#endif
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
