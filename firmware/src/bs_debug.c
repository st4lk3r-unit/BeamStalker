/*
 * bs_debug.c - Debug overlay implementation.
 *
 * Overlay geometry (top-right corner, scale=1 font):
 *   Width : OVL_W px
 *   Height: OVL_PAD*2 + nlines * LINE_H
 *   X     : bs_gfx_width() - OVL_W - 2
 *   Y     : 2
 *
 * Content (refreshed every REFRESH_MS):
 *   FPS:XX.X  UP:XXXs
 *   HEAP:XXXkB(minXXX)    - ESP32 only
 *   PSRAM:XXX/XXXkB        - ESP32 + PSRAM only
 *   CPU:XXXMHz             - ESP32 only
 *   DISP:WxH               - native only
 */
#include "bs/bs_debug.h"
#include "bs/bs_gfx.h"
#include "bs/bs_hw.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ---- overlay geometry ------------------------------------------------- */
#define OVL_W       136   /* px - wide enough for "HEAP:127kB(min98kB)" */
#define OVL_PAD       3   /* inner horizontal and vertical padding        */
#define LINE_H        9   /* text_h(1)=7 + 2px gap                        */
#define MAX_LINES     5
#define LLEN         24   /* max chars per stats line                      */
#define REFRESH_MS 2000

/* ---- state ------------------------------------------------------------- */
static const bs_arch_t* s_arch     = NULL;
static bool             s_enabled  = false;
static uint32_t         s_last_ms  = 0;
static uint32_t         s_frames   = 0;
static float            s_fps      = 0.0f;
static char             s_lines[MAX_LINES][LLEN];
static int              s_nlines   = 0;

/* ---- helpers ---------------------------------------------------------- */

static int ovl_h(void) { return OVL_PAD * 2 + s_nlines * LINE_H; }
static int ovl_x(void) { return bs_gfx_width() - OVL_W - 2; }
#define OVL_Y  2

static void refresh(void) {
    uint32_t now   = s_arch->millis();
    uint32_t delta = now - s_last_ms;

    s_fps     = (delta > 0) ? ((float)s_frames * 1000.0f / (float)delta) : 0.0f;
    s_frames  = 0;
    s_last_ms = now;

    bs_hw_info_t h;
    bs_hw_get_info(&h);

    s_nlines = 0;
    snprintf(s_lines[s_nlines++], LLEN, "FPS:%.1f UP:%.0fs",
             s_fps, (float)now / 1000.0f);

    if (h.heap_free_kb)
        snprintf(s_lines[s_nlines++], LLEN, "HEAP:%ukB(min%u)",
                 h.heap_free_kb, h.heap_min_kb);

    if (h.psram_total_kb)
        snprintf(s_lines[s_nlines++], LLEN, "PSRAM:%u/%ukB",
                 h.psram_free_kb, h.psram_total_kb);

    if (h.cpu_mhz)
        snprintf(s_lines[s_nlines++], LLEN, "CPU:%uMHz", h.cpu_mhz);
    else
        snprintf(s_lines[s_nlines++], LLEN, "DISP:%dx%d",
                 bs_gfx_width(), bs_gfx_height());
}

/* ---- public API ------------------------------------------------------- */

void bs_debug_init(const bs_arch_t* arch) {
    s_arch    = arch;
    s_enabled = false;
    s_frames  = 0;
    s_fps     = 0.0f;
    s_nlines  = 0;
    s_last_ms = arch ? arch->millis() : 0;
}

void bs_debug_set_enabled(bool en) {
    s_enabled = en;
    if (en && s_arch) {
        s_frames  = 0;
        s_last_ms = s_arch->millis();
        refresh();   /* populate lines immediately; fps will show 0 until first 2s */
    }
}

bool bs_debug_is_enabled(void) { return s_enabled; }

void bs_debug_frame(void) {
    if (!s_enabled || !s_arch) return;
    s_frames++;

    if (s_arch->millis() - s_last_ms >= REFRESH_MS)
        refresh();

    /* Draw background */
    int x = ovl_x();
    int h = ovl_h();
    bs_gfx_fill_rect(x, OVL_Y, OVL_W, h, BS_COL_BG);

    /* Border lines */
    bs_gfx_hline(x,         OVL_Y,         OVL_W, BS_COL_DIM);
    bs_gfx_hline(x,         OVL_Y + h - 1, OVL_W, BS_COL_DIM);

    /* Text lines */
    for (int i = 0; i < s_nlines; i++) {
        bs_gfx_text(x + OVL_PAD,
                    OVL_Y + OVL_PAD + i * LINE_H,
                    s_lines[i], BS_COL_BRIGHT, 1);
    }
}
