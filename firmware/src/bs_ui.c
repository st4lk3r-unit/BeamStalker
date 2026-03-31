/*
 * bs_ui.c - BeamStalker shared UI abstraction layer.
 *
 * Provides standard header/content layout helpers and persistent
 * settings (text scale, brightness) that survive app transitions.
 */
#include "bs/bs_ui.h"
#include "bs/bs_theme.h"
#include "bs/bs_menu.h"
#include "bs/bs_gfx.h"
#include "bs/bs_fs.h"
#include "bs/bs_hw.h"

#include <stdio.h>
#include <stdbool.h>

static int  s_text_scale    = 1;
static int  s_brightness    = 100;
static bool s_show_voltage  = false;
static int  s_grid_max_cols = 0;   /* 0=auto */
static int  s_grid_max_rows = 0;   /* 0=auto */

/* ---- Layout helpers --------------------------------------------------- */

int bs_ui_header_h(void) {
    return bs_gfx_text_h(s_text_scale) + 8;
}

void bs_ui_draw_header(const char* title) {
    int sw  = bs_gfx_width();
    int hh  = bs_ui_header_h();
    int ts  = s_text_scale;
    int ty  = (hh - bs_gfx_text_h(ts)) / 2;
    bs_gfx_fill_rect(0, 0, sw, hh, g_bs_theme.bg);
    bs_gfx_text(8, ty, title, g_bs_theme.dim, ts);

    /* Battery % — rate-limited read (every ~150 calls, same cadence as menu) */
    static int  s_bat_pct   = -1;
    static int  s_bat_timer = 0;
    if (s_bat_pct < 0 || --s_bat_timer <= 0) {
        s_bat_pct   = bs_hw_battery_pct();
        s_bat_timer = 150;
    }
    if (s_bat_pct > 0) {
        char bat_buf[8];
        snprintf(bat_buf, sizeof bat_buf, "%d%%", s_bat_pct);
        bs_color_t bat_col = (s_bat_pct < 20) ? g_bs_theme.warn : g_bs_theme.dim;
        int bw = bs_gfx_text_w(bat_buf, ts);
        bs_gfx_text(sw - bw - 4, ty, bat_buf, bat_col, ts);
    }

    bs_gfx_hline(0, hh - 1, sw, g_bs_theme.dim);
}

void bs_ui_draw_hint(const char* hint) {
    int sw = bs_gfx_width();
    int sh = bs_gfx_height();
    int ts = s_text_scale;
    /* Auto-downscale if hint overflows screen width */
    while (ts > 1 && bs_gfx_text_w(hint, ts) > sw - 16) ts--;
    int hw = bs_gfx_text_w(hint, ts);
    bs_gfx_text((sw - hw) / 2, sh - bs_gfx_text_h(ts) - 3, hint, g_bs_theme.dim, ts);
}

int bs_ui_content_y(void) {
    return bs_ui_header_h();
}

int bs_ui_content_h(void) {
    int ts = s_text_scale;
    /* Estimate hint height using auto-downscaled ts - use ts=1 as safe lower bound */
    return bs_gfx_height() - bs_ui_header_h() - bs_gfx_text_h(ts) - 6;
}

/* ---- Text scale ------------------------------------------------------- */

int bs_ui_text_scale(void) {
    return s_text_scale;
}

void bs_ui_set_text_scale(int s) {
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    s_text_scale = s;
}

/* ---- Show voltage ----------------------------------------------------- */

bool bs_ui_show_voltage(void)         { return s_show_voltage; }
void bs_ui_set_show_voltage(bool show) { s_show_voltage = show; }

/* ---- Grid max columns ------------------------------------------------- */

int  bs_ui_grid_max_cols(void)    { return s_grid_max_cols; }
void bs_ui_set_grid_max_cols(int n) {
    if (n != 0 && n < 2) n = 2;
    if (n > 4) n = 4;
    s_grid_max_cols = n;
}

/* ---- Grid max rows ---------------------------------------------------- */

int  bs_ui_grid_max_rows(void)    { return s_grid_max_rows; }
void bs_ui_set_grid_max_rows(int n) {
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    s_grid_max_rows = n;
}

/* ---- Brightness ------------------------------------------------------- */

int bs_ui_brightness(void) {
    return s_brightness;
}

void bs_ui_set_brightness(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_brightness = pct;
    bs_gfx_set_brightness(pct);
}

/* ---- Load settings from FS ------------------------------------------- */

void bs_ui_load_settings(void) {
    char buf[192]; size_t n = 0;
    if (bs_fs_read_file("settings.cfg", buf, sizeof buf - 1, &n) < 0) return;
    buf[n] = '\0';

    int palette_idx = 0, border_idx = -1, legacy_theme = -1;

    char* line = buf;
    while (*line) {
        char* nl = line; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = '\0';
        int val = 0;
        if (sscanf(line, "text_scale=%d",  &val) == 1) bs_ui_set_text_scale(val);
        if (sscanf(line, "brightness=%d",  &val) == 1) bs_ui_set_brightness(val);
        if (sscanf(line, "layout=%d",      &val) == 1) bs_menu_set_mode((bs_menu_mode_t)val);
        if (sscanf(line, "show_voltage=%d",&val) == 1) bs_ui_set_show_voltage(val != 0);
        if (sscanf(line, "grid_cols=%d",  &val) == 1) bs_ui_set_grid_max_cols(val);
        if (sscanf(line, "grid_rows=%d",  &val) == 1) bs_ui_set_grid_max_rows(val);
        /* Palette + border (new format) */
        if (sscanf(line, "palette=%d", &val) == 1) palette_idx   = val;
        if (sscanf(line, "border=%d",  &val) == 1) border_idx    = val;
        /* Legacy: theme=N used to be the only theme selector */
        if (sscanf(line, "theme=%d",   &val) == 1) legacy_theme  = val;
        *nl = save;
        line = nl + (*nl ? 1 : 0);
    }

    /* Apply appearance — prefer new palette/border, fall back to legacy theme */
    if (border_idx >= 0) {
        /* New format: palette + border independently */
        if (palette_idx < 0 || palette_idx >= BS_PALETTE_COUNT) palette_idx = 0;
        if (border_idx  < 0 || border_idx  >= BS_BORDER_STYLE_COUNT) border_idx = 0;
        bs_theme_apply(palette_idx, (bs_border_style_t)border_idx);
    } else if (legacy_theme >= 0 && legacy_theme < BS_THEME_COUNT) {
        /* Old format: full theme baked together */
        bs_theme_set(bs_themes[legacy_theme]);
    }
}
