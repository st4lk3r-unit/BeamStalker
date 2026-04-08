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

static float s_text_scale   = 1.0f;
static uint32_t s_ui_tick   = 0;
static int  s_brightness    = 100;
static bool s_show_voltage  = false;
static int  s_grid_max_cols = 0;   /* 0=auto */
static int  s_grid_max_rows = 0;   /* 0=auto */

/* ---- Layout helpers --------------------------------------------------- */

int bs_ui_header_h(void) {
    return bs_gfx_text_h(s_text_scale) + 8;
}

void bs_ui_draw_header(const char* title) {
    int sw   = bs_gfx_width();
    int hh   = bs_ui_header_h();
    float ts = s_text_scale;
    int ty   = (hh - bs_gfx_text_h(ts)) / 2;
    bs_gfx_fill_rect(0, 0, sw, hh, g_bs_theme.bg);
    /* Title: carousel if it overflows the available width */
    int bat_reserve = bs_gfx_text_w("100%", ts) + 8;
    bs_ui_draw_text_box(8, ty, sw - bat_reserve - 16, title, g_bs_theme.dim, ts);

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
    int sw    = bs_gfx_width();
    int sh    = bs_gfx_height();
    float ts  = s_text_scale;
    /* Auto-downscale if hint overflows screen width */
    while (ts > 1.0f && bs_gfx_text_w(hint, ts) > sw - 16) ts -= 0.5f;
    int hw = bs_gfx_text_w(hint, ts);
    bs_gfx_text((sw - hw) / 2, sh - bs_gfx_text_h(ts) - 3, hint, g_bs_theme.dim, ts);
}

int bs_ui_content_y(void) {
    return bs_ui_header_h();
}

int bs_ui_content_h(void) {
    float ts = s_text_scale;
    /* Estimate hint height using auto-downscaled ts - use ts=1 as safe lower bound */
    return bs_gfx_height() - bs_ui_header_h() - bs_gfx_text_h(ts) - 6;
}

/* ---- Text scale ------------------------------------------------------- */

float bs_ui_text_scale(void) {
    return s_text_scale;
}

void bs_ui_set_text_scale(float s) {
    if (s < 1.0f) s = 1.0f;
    if (s > 3.0f) s = 3.0f;
    s_text_scale = s;
}

/* ---- Carousel tick ---------------------------------------------------- */

void bs_ui_tick(void) {
    s_ui_tick++;
}

/* ---- Text box with carousel overflow ---------------------------------- */

/*
 * Draw text in a fixed-width box at (x, y).
 * If text fits in `w` pixels: drawn left-aligned.
 * If text overflows: animated marquee — slides left then right, pausing
 * at each end.  Animation is driven by s_ui_tick (bumped each frame via
 * bs_gfx_present() → bs_ui_tick()).
 */
void bs_ui_draw_text_box(int x, int y, int w,
                          const char* text, bs_color_t col, float scale) {
    if (!text || w <= 0) return;
    int tw = bs_gfx_text_w(text, scale);
    if (tw <= w) {
        bs_gfx_text(x, y, text, col, scale);
        return;
    }

    /* Scroll range: how many pixels the text must slide */
    int scroll_range = tw - w;

    /* Animation timing (all in "ticks" = frames):
     *   PAUSE  - hold at each end before sliding
     *   TPP    - ticks per pixel of scrolling (controls speed)
     */
    #define CAR_PAUSE 90       /* ~1.5 s at 60 fps */
    #define CAR_TPP    3       /* ~20 px/s at 60 fps */

    int slide = scroll_range * CAR_TPP;
    if (slide < 1) slide = 1;
    int cycle = 2 * CAR_PAUSE + 2 * slide;
    int phase = (int)(s_ui_tick % (uint32_t)cycle);

    int offset;
    if (phase < CAR_PAUSE) {
        offset = 0;
    } else if (phase < CAR_PAUSE + slide) {
        offset = (phase - CAR_PAUSE) / CAR_TPP;
    } else if (phase < 2 * CAR_PAUSE + slide) {
        offset = scroll_range;
    } else {
        offset = scroll_range - (phase - 2 * CAR_PAUSE - slide) / CAR_TPP;
    }

    #undef CAR_PAUSE
    #undef CAR_TPP

    /* Clip to box, draw at offset, restore full clip */
    bs_gfx_clip(x, y, w, bs_gfx_text_h(scale));
    bs_gfx_text(x - offset, y, text, col, scale);
    bs_gfx_clip(0, 0, 0, 0);
}

/* ---- Scroll arrows ---------------------------------------------------- */

void bs_ui_draw_scroll_arrows(int scroll, int total, int visible) {
    if (total <= visible) return;
    int sw = bs_gfx_width();
    int cy = bs_ui_content_y();
    int sh = bs_gfx_height();
    /* Use scale=1 for arrow glyphs so they stay small and don't eat layout */
    int th = bs_gfx_text_h(1.0f);
    int ax = sw - bs_gfx_text_w("^", 1.0f) - 3;
    if (scroll > 0)
        bs_gfx_text(ax, cy + 2, "^", g_bs_theme.dim, 1.0f);
    if (scroll + visible < total)
        bs_gfx_text(ax, sh - th - 3, "v", g_bs_theme.dim, 1.0f);
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
        int val = 0; float fval = 0.0f;
        if (sscanf(line, "text_scale=%f",  &fval) == 1) bs_ui_set_text_scale(fval);
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
