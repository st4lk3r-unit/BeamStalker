#pragma once
/*
 * bs_gfx.h - BeamStalker firmware-level graphics abstraction.
 *
 * Hides the rendering backend behind a common API:
 *   BS_GFX_NATIVE : ANSI full-screen terminal (native Linux debug)
 *   BS_USE_SGFX   : SGFX framebuffer → physical display (hardware)
 *   (neither)     : no-op stubs (UART-only boards)
 *
 * Coordinates are always in "screen units":
 *   SGFX   → pixels  (e.g. 480×222)
 *   native → character cells (80×24)
 *
 * Use bs_gfx_width() / bs_gfx_height() to stay resolution-agnostic.
 */
#include <stdint.h>
#include <stdbool.h>
#include "bs_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Color type -------------------------------------------------- */
typedef struct { uint8_t r, g, b, a; } bs_color_t;

static inline bs_color_t bs_col(uint8_t r, uint8_t g, uint8_t b) {
    bs_color_t c = {r, g, b, 0xFF}; return c;
}

/* ---------- Fallout-flavored palette ------------------------------------ */
/*  Black and orange - inspired by Pip-Boy / terminal aesthetics.          */
#define BS_COL_BG     bs_col(0x00, 0x00, 0x00)  /* terminal black          */
#define BS_COL_FG     bs_col(0xFF, 0x77, 0x00)  /* amber-orange (primary)  */
#define BS_COL_BRIGHT bs_col(0xFF, 0xAA, 0x22)  /* bright amber            */
#define BS_COL_DIM    bs_col(0x80, 0x38, 0x00)  /* dim orange              */
#define BS_COL_ACCENT bs_col(0xFF, 0xCC, 0x44)  /* pale gold accent        */
#define BS_COL_WARN   bs_col(0xFF, 0x30, 0x00)  /* danger red-orange       */
#define BS_COL_OK     bs_col(0xFF, 0x99, 0x00)  /* status ok (amber)       */

/* ---------- Lifecycle --------------------------------------------------- */
int  bs_gfx_init(const bs_arch_t* arch);
void bs_gfx_deinit(void);

/* ---------- Screen dimensions (screen-space units) --------------------- */
int  bs_gfx_width(void);
int  bs_gfx_height(void);

/* ---------- Border style ------------------------------------------------ */
typedef enum {
    BS_BORDER_SHARP   = 0,  /* solid 1px rectangle outline                    */
    BS_BORDER_BRACKET = 1,  /* corner brackets only — tactical HUD / crosshair */
    BS_BORDER_FILL    = 2,  /* filled rectangle — "top bar" selection style    */
} bs_border_style_t;

/* ---------- Drawing ----------------------------------------------------- */
void bs_gfx_clear(bs_color_t c);
void bs_gfx_fill_rect(int x, int y, int w, int h, bs_color_t c);
void bs_gfx_hline(int x, int y, int w, bs_color_t c);

/* Draw a border around (x,y,w,h). SHARP = solid 1px rect; BRACKET = corners only. */
void bs_gfx_border(int x, int y, int w, int h, bs_color_t c, bs_border_style_t style);

/* Draw a 1bpp packed bitmap. MSB=leftmost pixel, stride=ceil(w/8) bytes.
 * Set bits render as fg color; clear bits are transparent.
 * scale: each output pixel = scale×scale screen pixels (upscale, ≥1).
 * step:  sample every Nth source pixel in both axes (downscale, ≥1).
 *        step=1 → full resolution; step=2 → half size (60×60 from 120×120). */
void bs_gfx_bitmap_1bpp(int x, int y, int w, int h,
                         const uint8_t* data, bs_color_t fg, int scale, int step);

/*
 * Text - uses 5×7 bitmap font.
 * scale = 1.0 → smallest readable; scale = 3.0 → large banner.
 * Fractional scales (e.g. 1.5, 2.5) are supported and give intermediate sizes
 * via nearest-neighbour pixel stretching.
 * On SGFX: direct pixel fill using sgfx_font5x7_get().
 */
void bs_gfx_text(int x, int y, const char* s, bs_color_t c, float scale);
int  bs_gfx_text_w(const char* s, float scale);  /* advance width in screen units */
int  bs_gfx_text_h(float scale);                 /* line height in screen units   */

/* Set a clipping rectangle for all subsequent draw calls.
 * Pass w=0 or h=0 to disable clipping (full screen).
 * Used by bs_ui_draw_text_box() for carousel overflow. */
void bs_gfx_clip(int x, int y, int w, int h);

/* ---------- Present (flush to hardware / terminal) --------------------- */
void bs_gfx_present(void);

/* Set display brightness (0–100). No-op if not supported. */
void bs_gfx_set_brightness(int pct);

#ifdef __cplusplus
}
#endif
