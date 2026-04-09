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
#include "bs/bs_assets.h"
#include "beamstalker.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static float    s_text_scale   = 1.0f;
static uint32_t s_ui_ms        = 0;   /* carousel time accumulator (ms) */
static int      s_brightness   = 100;
static bool     s_show_voltage = false;
static int      s_header_brand_mode = 0; /* 0=text, 1=logo */
static int      s_grid_max_cols = 0;  /* 0=auto */
static int      s_grid_max_rows = 0;  /* 0=auto */
static bool     s_carousel     = false; /* off during boot; on after settings load */

/* ---- Layout helpers --------------------------------------------------- */


static bool bs_ui_use_header_logo(const char* title) {
    if (bs_ui_header_brand_mode() == 0) return false;
    if (title == NULL || *title == '\0') return true;
    return strcmp(title, BS_FW_NAME) == 0 || strcmp(title, "BeamStalker") == 0;
}

int bs_ui_header_h(void) {
    return bs_gfx_text_h(s_text_scale) + 8;
}

void bs_ui_draw_header(const char* title) {
    int sw   = bs_gfx_width();
    int hh   = bs_ui_header_h();
    float ts = s_text_scale;
    int ty   = (hh - bs_gfx_text_h(ts)) / 2;
    bs_gfx_fill_rect(0, 0, sw, hh, g_bs_theme.bg);

    /* Battery % — reserve width up-front so the brand area knows its cap. */
    int bat_reserve = bs_gfx_text_w("100%", ts) + 8;
    int title_x     = 8;
    int title_w     = sw - bat_reserve - 16;
    if (title_w < 8) title_w = 8;

    if (bs_ui_use_header_logo(title)) {
        /* Draw the real skull bitmap (120x120), scaled down to fit the header.
         * The previous code path used a 128x64 placeholder asset that is all zeros,
         * which is why the "Logo" setting appeared to do nothing. */
        int max_w = title_w;
        int max_h = hh - 4;
        if (max_w > 8 && max_h > 4) {
            int step_w = (120 + max_w - 1) / max_w;
            int step_h = (120 + max_h - 1) / max_h;
            int step   = step_w > step_h ? step_w : step_h;
            if (step < 1) step = 1;
            int out_h = (120 + step - 1) / step;
            int ox = title_x;
            int oy = (hh - out_h) / 2;
            bs_gfx_bitmap_1bpp(ox, oy, 120, 120, bs_skull_120, g_bs_theme.dim, 1, step);
        }
    } else {
        /* Title: carousel if it overflows the available width */
        bs_ui_draw_text_box(title_x, ty, title_w, title, g_bs_theme.dim, ts, true);
    }

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

/* ---- Carousel time advance -------------------------------------------- */

/* Called by app loops with the actual elapsed ms since the previous call.
 * This gives frame-rate-independent animation regardless of how long the
 * draw + SPI present takes on embedded hardware. */
void bs_ui_advance_ms(uint32_t ms) {
    s_ui_ms += ms;
}

/* Legacy weak-symbol target called by gfx backends — now a no-op since
 * app loops drive the clock via bs_ui_advance_ms(). */
void bs_ui_tick(void) { }

/* ---- Carousel enable/disable ------------------------------------------ */

bool bs_ui_carousel_enabled(void) { return s_carousel; }
void bs_ui_set_carousel(bool on)  { s_carousel = on; }

/* ---- Text box with carousel overflow ---------------------------------- */

/*
 * Draw text in a fixed-width box at (x, y).
 * If text fits in `w` pixels: drawn left-aligned.
 * If text overflows: animated marquee — slides left then right, pausing
 * at each end.  Animation is driven by s_ui_tick (bumped each frame via
 * bs_gfx_present() → bs_ui_tick()).
 */
void bs_ui_draw_text_box(int x, int y, int w,
                          const char* text, bs_color_t col, float scale, bool active) {
    if (!text || w <= 0) return;
    int tw = bs_gfx_text_w(text, scale);
    if (tw <= w) {
        bs_gfx_text(x, y, text, col, scale);
        return;
    }

    /* When inactive or carousel disabled: static clip at offset 0 */
    if (!active || !s_carousel) {
        bs_gfx_clip(x, y, w, bs_gfx_text_h(scale));
        bs_gfx_text(x, y, text, col, scale);
        bs_gfx_clip(0, 0, 0, 0);
        return;
    }

    /* Scroll range: how many pixels the text must slide */
    int scroll_range = tw - w;

    /* Animation timing (wall-clock milliseconds — frame-rate independent):
     *   PAUSE_MS   - hold at each end before sliding
     *   PX_PER_SEC - scrolling speed in pixels per second
     */
    #define CAR_PAUSE_MS    700   /* shorter pause at each end */
    #define CAR_PX_PER_SEC   80   /* slightly faster slide    */

    /* slide_ms: time to traverse the full scroll range */
    int slide_ms = (scroll_range * 1000) / CAR_PX_PER_SEC;
    if (slide_ms < 1) slide_ms = 1;
    int cycle_ms = 2 * CAR_PAUSE_MS + 2 * slide_ms;
    int phase_ms = (int)(s_ui_ms % (uint32_t)cycle_ms);

    int offset;
    if (phase_ms < CAR_PAUSE_MS) {
        offset = 0;
    } else if (phase_ms < CAR_PAUSE_MS + slide_ms) {
        offset = (phase_ms - CAR_PAUSE_MS) * scroll_range / slide_ms;
    } else if (phase_ms < 2 * CAR_PAUSE_MS + slide_ms) {
        offset = scroll_range;
    } else {
        offset = scroll_range
               - (phase_ms - 2 * CAR_PAUSE_MS - slide_ms) * scroll_range / slide_ms;
    }

    #undef CAR_PAUSE_MS
    #undef CAR_PX_PER_SEC

    /* Clip to box, draw at offset, restore full clip */
    bs_gfx_clip(x, y, w, bs_gfx_text_h(scale));
    bs_gfx_text(x - offset, y, text, col, scale);
    bs_gfx_clip(0, 0, 0, 0);
}

/* ---- Scroll arrows ---------------------------------------------------- */

/*
 * Draw a thin vertical scroll bar on the right edge of the content area.
 * A filled "thumb" rectangle indicates the current scroll position.
 * The track spans [content_y .. bottom-of-content]; the thumb is sized
 * proportionally to (visible / total) and positioned by (scroll / total).
 * No-op when all items fit (total <= visible).
 */
void bs_ui_draw_scroll_arrows(int scroll, int total, int visible) {
    if (total <= visible || total <= 0) return;

    int sw     = bs_gfx_width();
    int cy     = bs_ui_content_y();
    int sh     = bs_gfx_height();
    /* Hint line lives below content — measure it to stay above it */
    int hint_h = bs_gfx_text_h(s_text_scale) + 6;

    /* Track geometry: 2 px wide, 1 px from right edge */
    int bar_x  = sw - 3;
    int bar_y  = cy + 2;
    int bar_h  = sh - hint_h - cy - 4;
    if (bar_h < 8) return;

    /* Draw dim track */
    bs_gfx_fill_rect(bar_x, bar_y, 2, bar_h, g_bs_theme.dim);

    /* Thumb size (minimum 6 px so it's always visible) */
    int thumb_h = bar_h * visible / total;
    if (thumb_h < 6) thumb_h = 6;
    if (thumb_h > bar_h) thumb_h = bar_h;

    /* Thumb position: proportional, clamped so thumb stays inside track */
    int max_sc  = total - visible;
    if (max_sc < 1) max_sc = 1;
    int thumb_y = bar_y + (bar_h - thumb_h) * scroll / max_sc;
    if (thumb_y < bar_y) thumb_y = bar_y;
    if (thumb_y + thumb_h > bar_y + bar_h) thumb_y = bar_y + bar_h - thumb_h;

    bs_gfx_fill_rect(bar_x, thumb_y, 2, thumb_h, g_bs_theme.primary);
}

/* ---- Scroll clamp ----------------------------------------------------- */

void bs_ui_list_clamp_scroll(int cursor, int* scroll, int total, int visible) {
    if (cursor < *scroll)             *scroll = cursor;
    if (cursor >= *scroll + visible)  *scroll = cursor - visible + 1;
    if (*scroll < 0)                  *scroll = 0;
    int max_sc = total - visible;
    if (max_sc < 0) max_sc = 0;
    if (*scroll > max_sc)             *scroll = max_sc;
}

/* ---- Row height helpers ----------------------------------------------- */

int bs_ui_row_h(float ts) {
    return bs_gfx_text_h(ts) + 4;
}

int bs_ui_menu_row_h(float ts) {
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    return bs_gfx_text_h(ts) + bs_gfx_text_h(ts2) + 10;
}

int bs_ui_list_visible(float ts) {
    int vis = bs_ui_content_h() / bs_ui_row_h(ts);
    return vis < 1 ? 1 : vis;
}

int bs_ui_menu_visible(float ts) {
    int vis = bs_ui_content_h() / bs_ui_menu_row_h(ts);
    return vis < 1 ? 1 : vis;
}

/* ---- Compound row widgets --------------------------------------------- */

int bs_ui_draw_menu_row(int y, const char* name, const char* desc,
                         bool selected, float ts) {
    float ts2 = ts > 1.0f ? ts - 0.5f : 1.0f;
    int sw = bs_gfx_width();
    int lh = bs_ui_menu_row_h(ts);

    if (selected)
        bs_gfx_fill_rect(0, y - 3, sw, lh - 1, g_bs_theme.dim);

    bs_color_t nc = selected ? g_bs_theme.accent  : g_bs_theme.primary;
    bs_color_t dc = selected ? g_bs_theme.primary : g_bs_theme.dim;

    bs_ui_draw_text_box(8, y, sw - 16, name, nc, ts, selected);
    if (desc && desc[0])
        bs_ui_draw_text_box(8, y + bs_gfx_text_h(ts) + 2, sw - 16, desc, dc, ts2, selected);

    return lh;
}

int bs_ui_draw_kv_row(int y, const char* key, const char* val,
                       bool selected, bool warn, float ts) {
    int sw    = bs_gfx_width();
    int rh    = bs_ui_row_h(ts);
    int pad_y = (rh - bs_gfx_text_h(ts)) / 2;
    int vx    = sw / 2;
    int avail_l = vx - 14;      /* 6 px left margin, 8 px text indent */
    int avail_v = sw - vx - 4;

    if (selected)
        bs_gfx_fill_rect(0, y - 1, sw, rh, g_bs_theme.dim);

    bs_color_t lc = selected ? g_bs_theme.secondary : g_bs_theme.dim;
    bs_color_t vc = warn     ? g_bs_theme.warn
                  : selected ? g_bs_theme.accent
                             : g_bs_theme.primary;

    if (avail_l > 0)
        bs_ui_draw_text_box(6, y + pad_y, avail_l, key, lc, ts, selected);
    if (avail_v > 0 && val)
        bs_ui_draw_text_box(vx, y + pad_y, avail_v, val, vc, ts, selected);

    return rh;
}

/* ---- Show voltage ----------------------------------------------------- */

bool bs_ui_show_voltage(void)         { return s_show_voltage; }
void bs_ui_set_show_voltage(bool show) { s_show_voltage = show; }

/* ---- Header brand mode ----------------------------------------------- */

int  bs_ui_header_brand_mode(void) { return s_header_brand_mode; }
void bs_ui_set_header_brand_mode(int mode) { s_header_brand_mode = (mode != 0) ? 1 : 0; }

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
    if (bs_fs_read_file("settings.cfg", buf, sizeof buf - 1, &n) < 0) {
        /* No settings file: enable carousel (new-install default) */
        s_carousel = true;
        return;
    }
    buf[n] = '\0';

    int palette_idx = 0, border_idx = -1, legacy_theme = -1;
    bool carousel_set = false;

    char* line = buf;
    while (*line) {
        char* nl = line; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = '\0';
        int val = 0; float fval = 0.0f;
        if (sscanf(line, "text_scale=%f",  &fval) == 1) bs_ui_set_text_scale(fval);
        if (sscanf(line, "brightness=%d",  &val) == 1) bs_ui_set_brightness(val);
        if (sscanf(line, "layout=%d",      &val) == 1) bs_menu_set_mode((bs_menu_mode_t)val);
        if (sscanf(line, "show_voltage=%d",&val) == 1) bs_ui_set_show_voltage(val != 0);
        if (sscanf(line, "header_brand=%d",&val) == 1) bs_ui_set_header_brand_mode(val);
        if (sscanf(line, "grid_cols=%d",  &val) == 1) bs_ui_set_grid_max_cols(val);
        if (sscanf(line, "grid_rows=%d",  &val) == 1) bs_ui_set_grid_max_rows(val);
        if (sscanf(line, "carousel=%d",   &val) == 1) {
            bs_ui_set_carousel(val != 0);
            carousel_set = true;
        }
        /* Palette + border (new format) */
        if (sscanf(line, "palette=%d", &val) == 1) palette_idx   = val;
        if (sscanf(line, "border=%d",  &val) == 1) border_idx    = val;
        /* Legacy: theme=N used to be the only theme selector */
        if (sscanf(line, "theme=%d",   &val) == 1) legacy_theme  = val;
        *nl = save;
        line = nl + (*nl ? 1 : 0);
    }

    /* If settings file exists but has no carousel= line, default to on */
    if (!carousel_set) s_carousel = true;

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
