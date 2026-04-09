/*
 * bs_gfx_sgfx.c - bs_gfx backend for hardware displays via SGFX.
 *
 * Uses the SGFX framebuffer + presenter pattern:
 *   - Persistent sgfx_fb_t / sgfx_present_t (allocated once at init)
 *   - Each draw call modifies the fb and marks dirty tiles
 *   - bs_gfx_present() → sgfx_present_frame() pushes only changed tiles
 *
 * Text rendering: sgfx_font5x7_get() + manual pixel fill into FB.
 * This matches the example_wrapup pattern and works reliably on all panels.
 *
 * Requires: -DBS_USE_SGFX, -DSGFX_W, -DSGFX_H, and bus/driver flags.
 */
#ifdef BS_USE_SGFX

#include "bs/bs_gfx.h"
#include "sgfx.h"
#include "sgfx_port.h"
#include "sgfx_fb.h"
#include "sgfx_font_builtin.h"

#include <string.h>
#include <stdlib.h>

/* ---- scratch buffer for SGFX HAL (bus DMA staging) ------------------- */
#ifndef BS_SGFX_SCRATCH_BYTES
#  ifdef SGFX_SCRATCH_BYTES
#    define BS_SGFX_SCRATCH_BYTES SGFX_SCRATCH_BYTES
#  else
#    define BS_SGFX_SCRATCH_BYTES 8192
#  endif
#endif

static uint8_t        s_scratch[BS_SGFX_SCRATCH_BYTES];
static sgfx_device_t  s_dev;
static sgfx_fb_t      s_fb;
static sgfx_present_t s_pr;
static int            s_w, s_h;
static bool           s_ready = false;

/* Clip rectangle — (0,0,0,0) = disabled */
static int s_clip_x, s_clip_y, s_clip_w, s_clip_h;

#if defined(VARIANT_CARDPUTER)
static void bs_cardputer_panel_fixup(void) {
    if (!s_dev.bus || !s_dev.bus->ops || !s_dev.bus->ops->write_cmd) return;
    (void)s_dev.bus->ops->write_cmd(s_dev.bus, 0x21); /* INVON */
}
#else
static void bs_cardputer_panel_fixup(void) {}
#endif

#if defined(VARIANT_TDONGLE_S3)
static void bs_tdongle_s3_panel_fixup(void) {
    if (!s_dev.bus || !s_dev.bus->ops) return;
    if (s_dev.bus->ops->gpio_set) {
#if defined(BS_SGFX_BL_ACTIVE_LOW) && BS_SGFX_BL_ACTIVE_LOW
        (void)s_dev.bus->ops->gpio_set(s_dev.bus, 3 /* BL */, 0);
#else
        (void)s_dev.bus->ops->gpio_set(s_dev.bus, 3 /* BL */, 1);
#endif
    }
}
#else
static void bs_tdongle_s3_panel_fixup(void) {}
#endif

/* ---- internal FB helpers --------------------------------------------- */

static inline uint16_t pack565(bs_color_t c) {
    return (uint16_t)(((c.r & 0xF8u) << 8) | ((c.g & 0xFCu) << 3) | (c.b >> 3));
}

static void fb_fill(int x, int y, int w, int h, bs_color_t c) {
    if (!s_fb.px || w <= 0 || h <= 0) return;
    /* Apply clip rect if active */
    if (s_clip_w > 0 && s_clip_h > 0) {
        int cx2 = s_clip_x + s_clip_w, cy2 = s_clip_y + s_clip_h;
        if (x < s_clip_x) { w -= (s_clip_x - x); x = s_clip_x; }
        if (y < s_clip_y) { h -= (s_clip_y - y); y = s_clip_y; }
        if (x + w > cx2)  w = cx2 - x;
        if (y + h > cy2)  h = cy2 - y;
        if (w <= 0 || h <= 0) return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;

    uint16_t v = pack565(c);
    for (int j = 0; j < h; j++) {
        uint16_t* row = (uint16_t*)((uint8_t*)s_fb.px + (size_t)(y + j) * (size_t)s_fb.stride) + x;
        for (int i = 0; i < w; i++) row[i] = v;
    }
    sgfx_fb_mark_dirty_px(&s_fb, x, y, w, h);
}

static void fb_clear_all(bs_color_t c) {
    fb_fill(0, 0, s_w, s_h, c);
}

/* ---- 5×7 text into framebuffer --------------------------------------- */

/*
 * Each glyph: 5 columns, 7 rows.  advance = ceil(6 * scale) px.
 * Fractional scale (e.g. 1.5) renders each font pixel as a rectangle
 * whose size is determined by the next-pixel-boundary method — pixels
 * alternate between 1px and 2px so no gaps or overlaps occur.
 */
static void fb_text5x7(int x0, int y0, const char* s, bs_color_t c, float scale) {
    if (!s_fb.px || !s || scale < 0.5f) return;
    int adv = (int)(6.0f * scale);
    if (adv < 1) adv = 1;
    for (; *s; ++s, x0 += adv) {
        uint8_t cols[5];
        if (!sgfx_font5x7_get(*s, cols)) continue;
        for (int col = 0; col < 5; col++) {
            int px = x0 + (int)((float)col * scale);
            int pw = (int)((float)(col + 1) * scale) - (int)((float)col * scale);
            if (pw < 1) pw = 1;
            for (int row = 0; row < 7; row++) {
                if (cols[col] & (uint8_t)(1u << row)) {
                    int py = y0 + (int)((float)row * scale);
                    int ph = (int)((float)(row + 1) * scale) - (int)((float)row * scale);
                    if (ph < 1) ph = 1;
                    fb_fill(px, py, pw, ph, c);
                }
            }
        }
    }
}

/* ---- bs_gfx API ------------------------------------------------------- */

int bs_gfx_init(const bs_arch_t* arch) {
    (void)arch;
    int rc = sgfx_autoinit(&s_dev, s_scratch, sizeof s_scratch);
    if (rc) return rc;

    bs_cardputer_panel_fixup();
    bs_tdongle_s3_panel_fixup();

    s_w = (int)s_dev.caps.width;
    s_h = (int)s_dev.caps.height;

    rc = sgfx_fb_create(&s_fb, s_w, s_h, 16, 16);
    if (rc || !s_fb.px) {
        /* Fallback: no PSRAM - graceful degradation (direct draw, no partial updates) */
        s_ready = false;
        return -1;
    }
    sgfx_present_init(&s_pr, s_w);

    /* Clear to black */
    fb_clear_all(BS_COL_BG);
    sgfx_present_frame(&s_pr, &s_dev, &s_fb);

    s_ready = true;
    return 0;
}

void bs_gfx_deinit(void) {
    if (s_ready) {
        sgfx_present_deinit(&s_pr);
        sgfx_fb_destroy(&s_fb);
        s_ready = false;
    }
}

int bs_gfx_width(void)  { return s_w; }
int bs_gfx_height(void) { return s_h; }

void bs_gfx_clear(bs_color_t c) {
    if (!s_ready) return;
    fb_clear_all(c);
}

void bs_gfx_fill_rect(int x, int y, int w, int h, bs_color_t c) {
    if (!s_ready) return;
    fb_fill(x, y, w, h, c);
}

void bs_gfx_hline(int x, int y, int w, bs_color_t c) {
    if (!s_ready) return;
    fb_fill(x, y, w, 1, c);
}

void bs_gfx_bitmap_1bpp(int x, int y, int w, int h,
                          const uint8_t* data, bs_color_t fg, int scale, int step) {
    if (!s_ready || !data || scale < 1) return;
    if (step < 1) step = 1;
    int stride = (w + 7) / 8;
    int out_h = (h + step - 1) / step;
    int out_w = (w + step - 1) / step;
    for (int oy = 0; oy < out_h; oy++) {
        for (int ox = 0; ox < out_w; ox++) {
            int hit = 0;
            for (int sy = 0; sy < step && !hit; sy++) {
                int row = oy * step + sy;
                if (row >= h) break;
                for (int sx = 0; sx < step && !hit; sx++) {
                    int col = ox * step + sx;
                    if (col >= w) break;
                    if ((data[row * stride + col / 8] >> (7 - (col & 7))) & 1u)
                        hit = 1;
                }
            }
            if (hit)
                fb_fill(x + ox * scale, y + oy * scale, scale, scale, fg);
        }
    }
}

void bs_gfx_text(int x, int y, const char* s, bs_color_t c, float scale) {
    if (!s_ready) return;
    if (scale < 0.5f) scale = 0.5f;
    fb_text5x7(x, y, s, c, scale);
}

int bs_gfx_text_w(const char* s, float scale) {
    if (!s || scale < 0.5f) return 0;
    int n = 0;
    while (*s++) n++;
    return (int)((float)n * 6.0f * scale);
}

int bs_gfx_text_h(float scale) {
    return (int)(7.0f * (scale < 1.0f ? 1.0f : scale));
}

void bs_gfx_clip(int x, int y, int w, int h) {
    s_clip_x = x; s_clip_y = y; s_clip_w = w; s_clip_h = h;
}

/* Weak reference — bs_ui.c provides the strong version for carousel animation */
__attribute__((weak)) void bs_ui_tick(void) {}

void bs_gfx_present(void) {
    if (!s_ready) return;
    sgfx_present_frame(&s_pr, &s_dev, &s_fb);
    bs_ui_tick();
}

/*
 * bs_gfx_backlight_hw - platform-specific backlight control.
 * Weak no-op; override in a dedicated backlight driver file
 * (e.g. bs_gfx_backlight_aw9364.c for the T-Pager AW9364 chip).
 */
__attribute__((weak)) void bs_gfx_backlight_hw(int pct) { (void)pct; }

void bs_gfx_set_brightness(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    bs_gfx_backlight_hw(pct);
}

#endif /* BS_USE_SGFX */
