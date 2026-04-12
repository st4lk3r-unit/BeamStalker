/*
 * bs_gfx_native.c - bs_gfx backend for native Linux (SDL2 window).
 *
 * Opens a dedicated SDL2 window that simulates the hardware display.
 * The current terminal (stdin/stdout) is left entirely for the konsole
 * (UART simulation), exactly as it would be on real hardware.
 *
 * Display model:
 *   Logical resolution : DISP_W × DISP_H pixels  (defaults: 480 × 222)
 *   SDL window size    : DISP_W×WIN_SCALE × DISP_H×WIN_SCALE (nearest-
 *                        neighbour scaled, crisp pixel art appearance)
 *
 * Rendering uses a persistent render-target texture (s_fb) so that
 * callers can erase a small region and redraw - identical to the SGFX
 * framebuffer + presenter pattern on hardware.
 *
 * bs_gfx_width()  → DISP_W  (480)
 * bs_gfx_height() → DISP_H  (222)
 *
 * These match the T-Pager hardware dimensions, so layout and scale
 * constants in higher-level code need no platform ifdefs.
 */
#ifdef BS_GFX_NATIVE

#include "bs/bs_gfx.h"
#include "sgfx_font_builtin.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

/* ---- Compile-time display geometry ------------------------------------ */

#ifndef BS_NATIVE_W
#  define BS_NATIVE_W   480
#endif
#ifndef BS_NATIVE_H
#  define BS_NATIVE_H   222
#endif
#ifndef BS_NATIVE_SCALE
#  define BS_NATIVE_SCALE  2   /* SDL window = logical × SCALE (nearest-neighbour) */
#endif

#define DISP_W     BS_NATIVE_W
#define DISP_H     BS_NATIVE_H
#define WIN_SCALE  BS_NATIVE_SCALE

/* ---- SDL objects ------------------------------------------------------- */

static SDL_Window*   s_win      = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_Texture*  s_fb       = NULL;   /* persistent render-target framebuffer */

/* ---- bs_gfx API ------------------------------------------------------- */

int bs_gfx_init(const bs_arch_t* arch) {
    (void)arch;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) return -1;

    s_win = SDL_CreateWindow(
        "BeamStalker Display",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISP_W * WIN_SCALE, DISP_H * WIN_SCALE,
        SDL_WINDOW_SHOWN);
    if (!s_win) return -1;

    s_renderer = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!s_renderer) return -1;

    /* Nearest-neighbour filtering - keeps pixel art crisp when scaled */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    /* Persistent framebuffer texture: DISP_W×DISP_H logical pixels */
    s_fb = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET,
        DISP_W, DISP_H);
    if (!s_fb) return -1;

    /* All subsequent draw calls target the framebuffer texture */
    SDL_SetRenderTarget(s_renderer, s_fb);

    /* Initial clear to black */
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);

    return 0;
}

void bs_gfx_deinit(void) {
    if (s_fb)       { SDL_DestroyTexture(s_fb);      s_fb       = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_win)      { SDL_DestroyWindow(s_win);        s_win      = NULL; }
    SDL_Quit();
}

int bs_gfx_width(void)  { return DISP_W; }
int bs_gfx_height(void) { return DISP_H; }

void bs_gfx_clear(bs_color_t c) {
    if (!s_renderer) return;
    SDL_SetRenderDrawColor(s_renderer, c.r, c.g, c.b, 255);
    SDL_RenderClear(s_renderer);
}

void bs_gfx_fill_rect(int x, int y, int w, int h, bs_color_t c) {
    if (!s_renderer) return;
    SDL_Rect r = {x, y, w, h};
    SDL_SetRenderDrawColor(s_renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(s_renderer, &r);
}

void bs_gfx_hline(int x, int y, int w, bs_color_t c) {
    if (!s_renderer) return;
    SDL_Rect r = {x, y, w, 1};
    SDL_SetRenderDrawColor(s_renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(s_renderer, &r);
}

void bs_gfx_bitmap_1bpp(int x, int y, int w, int h,
                          const uint8_t* data, bs_color_t fg, int scale, int step) {
    if (!s_renderer || !data || scale < 1) return;
    if (step < 1) step = 1;
    int stride = (w + 7) / 8;
    SDL_SetRenderDrawColor(s_renderer, fg.r, fg.g, fg.b, 255);
    int out_h = (h + step - 1) / step;
    int out_w = (w + step - 1) / step;
    for (int oy = 0; oy < out_h; oy++) {
        for (int ox = 0; ox < out_w; ox++) {
            /* OR-reduce: output pixel set if ANY source pixel in the block is set.
             * Preserves thin features that pure subsampling would drop. */
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
            if (hit) {
                SDL_Rect r = {x + ox * scale, y + oy * scale, scale, scale};
                SDL_RenderFillRect(s_renderer, &r);
            }
        }
    }
}

/*
 * Text rendering: 5×7 bitmap font → filled rectangles.
 * Fractional scale (e.g. 1.5) uses next-pixel-boundary sizing so font
 * pixels alternate between 1px and 2px — no gaps, no overlaps.
 */
void bs_gfx_text(int x, int y, const char* s, bs_color_t c, float scale) {
    if (!s_renderer || !s || scale < 0.5f) return;
    SDL_SetRenderDrawColor(s_renderer, c.r, c.g, c.b, 255);
    int adv = (int)(6.0f * scale);
    if (adv < 1) adv = 1;
    for (; *s; ++s, x += adv) {
        uint8_t font_cols[5];
        if (!sgfx_font5x7_get(*s, font_cols)) continue;
        for (int col = 0; col < 5; col++) {
            int px = x + (int)((float)col * scale);
            int pw = (int)((float)(col + 1) * scale) - (int)((float)col * scale);
            if (pw < 1) pw = 1;
            for (int row = 0; row < 7; row++) {
                if (!((font_cols[col] >> row) & 1u)) continue;
                int py = y + (int)((float)row * scale);
                int ph = (int)((float)(row + 1) * scale) - (int)((float)row * scale);
                if (ph < 1) ph = 1;
                SDL_Rect r = {px, py, pw, ph};
                SDL_RenderFillRect(s_renderer, &r);
            }
        }
    }
}

int bs_gfx_text_w(const char* s, float scale) {
    if (!s || scale < 0.5f) return 0;
    int n = 0; while (*s++) n++;
    return (int)((float)n * 6.0f * scale);
}

int bs_gfx_text_h(float scale) {
    return (int)(7.0f * (scale < 1.0f ? 1.0f : scale));
}

void bs_gfx_clip(int x, int y, int w, int h) {
    if (!s_renderer) return;
    if (w <= 0 || h <= 0) {
        SDL_RenderSetClipRect(s_renderer, NULL);
    } else {
        SDL_Rect r = {x, y, w, h};
        SDL_RenderSetClipRect(s_renderer, &r);
    }
}

/* Weak reference — bs_ui.c provides the strong version for carousel animation */
__attribute__((weak)) void bs_ui_tick(void) {}

/*
 * Present: copy framebuffer texture to window (scaled) and flip.
 * Pumps SDL events - QUIT causes immediate exit.
 * Render target is restored to s_fb so subsequent draw calls work.
 */
void bs_gfx_present(void) {
    if (!s_renderer) return;

    /* Disable clip before blitting to window */
    SDL_RenderSetClipRect(s_renderer, NULL);

    /* Blit framebuffer to window surface */
    SDL_SetRenderTarget(s_renderer, NULL);
    SDL_RenderCopy(s_renderer, s_fb, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    /* Restore render target for next frame's draw calls */
    SDL_SetRenderTarget(s_renderer, s_fb);

    /* Keep window responsive; QUIT event exits cleanly */
    SDL_PumpEvents();
    if (SDL_HasEvent(SDL_QUIT)) exit(0);

    bs_ui_tick();
}

void bs_gfx_set_brightness(int pct) { (void)pct; /* SDL window, no backlight control */ }

#endif /* BS_GFX_NATIVE */
