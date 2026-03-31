/*
 * bs_gfx_utils.c - Shared drawing utilities built on top of bs_gfx primitives.
 *
 * Compiled for both backends (native + SGFX hardware).  Has no backend-specific
 * code — only calls bs_gfx_hline() and bs_gfx_fill_rect() which both backends
 * provide.
 */
#if defined(BS_GFX_NATIVE) || defined(BS_USE_SGFX)

#include "bs/bs_gfx.h"

void bs_gfx_border(int x, int y, int w, int h, bs_color_t c, bs_border_style_t style) {
    if (w <= 0 || h <= 0) return;

    if (style == BS_BORDER_FILL) {
        /* Filled background — solid color rectangle, like a header-bar highlight */
        bs_gfx_fill_rect(x, y, w, h, c);
    } else if (style == BS_BORDER_BRACKET) {
        /*
         * 2px-thick corner brackets — targeting-reticle / tactical HUD look.
         * Arm length ≈30% of shortest side, clamped to [4..16].
         * Each arm is a 2-pixel-wide fill_rect so corners look bold.
         */
        int len = (w < h ? w : h) * 3 / 10;
        if (len < 4)      len = 4;
        if (len > 16)     len = 16;
        if (len > w / 2)  len = w / 2;
        if (len > h / 2)  len = h / 2;

        /* Top-left */
        bs_gfx_fill_rect(x,         y,         len, 2, c);
        bs_gfx_fill_rect(x,         y,         2,  len, c);
        /* Top-right */
        bs_gfx_fill_rect(x + w - len, y,       len, 2, c);
        bs_gfx_fill_rect(x + w - 2,   y,       2,  len, c);
        /* Bottom-left */
        bs_gfx_fill_rect(x,         y + h - 2, len, 2, c);
        bs_gfx_fill_rect(x,         y + h - len, 2, len, c);
        /* Bottom-right */
        bs_gfx_fill_rect(x + w - len, y + h - 2, len, 2, c);
        bs_gfx_fill_rect(x + w - 2,   y + h - len, 2, len, c);
    } else {
        /* BS_BORDER_SHARP: solid 1px rectangle */
        bs_gfx_hline    (x,         y,         w, c);
        bs_gfx_hline    (x,         y + h - 1, w, c);
        bs_gfx_fill_rect(x,         y,         1, h, c);
        bs_gfx_fill_rect(x + w - 1, y,         1, h, c);
    }
}

#endif /* BS_GFX_NATIVE || BS_USE_SGFX */
