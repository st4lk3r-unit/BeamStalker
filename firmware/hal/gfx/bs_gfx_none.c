/*
 * bs_gfx_none.c - No-op graphics backend for headless / UART-only builds.
 *
 * Activated when neither BS_GFX_NATIVE nor BS_USE_SGFX is defined.
 * All draw calls are silent no-ops.  Width/height return a virtual size so
 * apps that still call layout helpers (bs_gfx_text_h, etc.) don't crash.
 *
 * Build flag:  define nothing — absence of BS_GFX_NATIVE and BS_USE_SGFX
 *              activates this backend automatically.
 */
#if !defined(BS_GFX_NATIVE) && !defined(BS_USE_SGFX)

#include "bs/bs_gfx.h"
#include <stddef.h>
#include <string.h>

/* Virtual dimensions — kept reasonable so layout math doesn't divide by zero */
#ifndef BS_HEADLESS_W
#  define BS_HEADLESS_W 480
#endif
#ifndef BS_HEADLESS_H
#  define BS_HEADLESS_H 222
#endif

int  bs_gfx_init(const bs_arch_t* arch) { (void)arch; return 0; }
void bs_gfx_deinit(void) {}

int  bs_gfx_width(void)  { return BS_HEADLESS_W; }
int  bs_gfx_height(void) { return BS_HEADLESS_H; }

void bs_gfx_clear(bs_color_t c)                              { (void)c; }
void bs_gfx_fill_rect(int x, int y, int w, int h, bs_color_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void bs_gfx_hline(int x, int y, int w, bs_color_t c)        { (void)x;(void)y;(void)w;(void)c; }
void bs_gfx_border(int x, int y, int w, int h, bs_color_t c,
                   bs_border_style_t s) { (void)x;(void)y;(void)w;(void)h;(void)c;(void)s; }
void bs_gfx_bitmap_1bpp(int x, int y, int w, int h,
                         const uint8_t* data, bs_color_t fg,
                         int scale, int step) {
    (void)x;(void)y;(void)w;(void)h;(void)data;(void)fg;(void)scale;(void)step;
}
void bs_gfx_text(int x, int y, const char* s, bs_color_t c, float scale) {
    (void)x;(void)y;(void)s;(void)c;(void)scale;
}
/* Return approximate metrics so apps can compute layout without rendering */
int  bs_gfx_text_w(const char* s, float scale) {
    if (!s) return 0;
    int len = 0; while (s[len]) len++;
    return (int)((float)len * 6.0f * scale);
}
int  bs_gfx_text_h(float scale) { return (int)(7.0f * (scale < 1.0f ? 1.0f : scale)); }

void bs_gfx_clip(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }

/* Weak reference — bs_ui.c provides the strong version for carousel animation */
__attribute__((weak)) void bs_ui_tick(void) {}

void bs_gfx_present(void) { bs_ui_tick(); }
void bs_gfx_set_brightness(int pct) { (void)pct; }

#endif /* !BS_GFX_NATIVE && !BS_USE_SGFX */
