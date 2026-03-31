/*
 * app_log.c - Scrollable in-memory log viewer.
 *
 * Shows all ring buffer entries (boot to now) at the current text scale.
 * Long entries are word-wrapped to fit the screen width.
 *
 * Navigation:
 *   UP / PREV   → scroll up one entry
 *   DOWN / NEXT → scroll down one entry
 *   LEFT        → page up
 *   RIGHT       → page down
 *   BACK        → exit
 *
 * The view starts at the bottom (newest entries visible), matching the
 * boot log auto-scroll behaviour.
 */
#include "apps/app_log.h"
#include "bs/bs_log.h"
#include "bs/bs_gfx.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_nav.h"
#include "bs/bs_arch.h"

#include <string.h>
#include <stdio.h>

/* 16×16 "document with text lines" icon - 2 bytes per row */
static const uint8_t k_log_icon_16[] = {
    0xFF, 0xFF,  /* ████████████████  border */
    0x80, 0x01,  /* █              █  */
    0x80, 0x01,  /* █              █  */
    0xBF, 0xF1,  /* █ ███████████  █  long line */
    0x80, 0x01,  /* █              █  */
    0xBF, 0x81,  /* █ ████████     █  medium line */
    0x80, 0x01,  /* █              █  */
    0xBF, 0xF1,  /* █ ███████████  █  long line */
    0x80, 0x01,  /* █              █  */
    0xBE, 0x01,  /* █ █████        █  short line */
    0x80, 0x01,  /* █              █  */
    0xBF, 0xF1,  /* █ ███████████  █  long line */
    0x80, 0x01,  /* █              █  */
    0xBF, 0x81,  /* █ ████████     █  medium line */
    0x80, 0x01,  /* █              █  */
    0xFF, 0xFF,  /* ████████████████  border */
};

/* ---- Wrap helpers ------------------------------------------------------ */

/*
 * chars_per_row - how many characters fit in text_w pixels at scale ts.
 * Uses a single-char probe; all built-in fonts are monospace.
 */
static int chars_per_row(int text_w, int ts) {
    int cw = bs_gfx_text_w("W", ts);
    if (cw <= 0) cw = 6;
    int cpr = text_w / cw;
    return (cpr < 1) ? 1 : cpr;
}

/*
 * entry_rows - number of display rows a single log entry occupies when wrapped.
 */
static int entry_rows(const char* text, int cpr) {
    int len = (int)strlen(text);
    if (len == 0) return 1;
    return (len + cpr - 1) / cpr;
}

/* ---- Draw -------------------------------------------------------------- */

static void draw_log_view(int scroll) {
    int ts      = bs_ui_text_scale();
    int sw      = bs_gfx_width();
    int sh      = bs_gfx_height();
    int hh      = bs_ui_header_h();
    int line_h  = bs_gfx_text_h(ts) + 2;
    int visible = (sh - hh) / line_h;
    int count   = bs_log_count();
    int cpr     = chars_per_row(sw - 4, ts);   /* chars per display row */

    bs_gfx_clear(g_bs_theme.bg);

    /* Header */
    char title[32];
    snprintf(title, sizeof title, "Log  %d entries", count);
    int ty = (hh - bs_gfx_text_h(ts)) / 2;
    bs_gfx_text(8, ty, title, g_bs_theme.primary, ts);
    bs_gfx_hline(0, hh - 1, sw, g_bs_theme.dim);

    /* Entries — rendered with line wrap */
    int display_row = 0;
    int last_i      = scroll - 1;   /* track highest entry actually rendered */

    for (int i = scroll; i < count && display_row < visible; i++) {
        const char* entry = bs_log_entry(i);
        if (!entry) { display_row++; continue; }

        /* Strip trailing newline */
        char buf[256];
        int elen = (int)strlen(entry);
        if (elen > 0 && entry[elen - 1] == '\n') elen--;
        if (elen > (int)sizeof(buf) - 1) elen = (int)sizeof(buf) - 1;
        memcpy(buf, entry, (size_t)elen);
        buf[elen] = '\0';

        bs_color_t col = bs_log_level_color(bs_log_entry_lvl(i));

        int offset = 0;
        do {
            if (display_row >= visible) break;
            int chunk = elen - offset;
            if (chunk > cpr) chunk = cpr;
            char tmp[256];
            memcpy(tmp, buf + offset, (size_t)chunk);
            tmp[chunk] = '\0';
            bs_gfx_text(2, hh + display_row * line_h, tmp, col, ts);
            display_row++;
            offset += chunk;
        } while (offset < elen);

        last_i = i;
    }

    /* Scroll indicators */
    if (scroll > 0) {
        int iw = bs_gfx_text_w("^ more", 1);
        bs_gfx_text(sw - iw - 4, hh + 2, "^ more", g_bs_theme.dim, 1);
    }
    if (last_i < count - 1) {
        int iw = bs_gfx_text_w("v more", 1);
        bs_gfx_text(sw - iw - 4, sh - bs_gfx_text_h(1) - 3, "v more", g_bs_theme.dim, 1);
    }
}

/* ---- App --------------------------------------------------------------- */

static void app_log_run(const bs_arch_t* arch) {
    int ts      = bs_ui_text_scale();
    int sh      = bs_gfx_height();
    int hh      = bs_ui_header_h();
    int line_h  = bs_gfx_text_h(ts) + 2;
    int visible = (sh - hh) / line_h;
    int count   = bs_log_count();

    /* Start at the bottom — find first entry such that remaining entries fill view */
    int scroll = count - visible;
    if (scroll < 0) scroll = 0;

    bool dirty = true;

    while (true) {
        count = bs_log_count();

        if (dirty) {
            draw_log_view(scroll);
            bs_gfx_present();
            dirty = false;
        }

        arch->delay_ms(5);
        bs_nav_id_t nav = bs_nav_poll();

        switch (nav) {
            case BS_NAV_BACK:
                return;

            case BS_NAV_UP:
            case BS_NAV_PREV:
                if (scroll > 0) { scroll--; dirty = true; }
                break;

            case BS_NAV_DOWN:
            case BS_NAV_NEXT:
                if (scroll < count - 1) { scroll++; dirty = true; }
                break;

            case BS_NAV_LEFT:   /* page up */
                scroll -= visible;
                if (scroll < 0) scroll = 0;
                dirty = true;
                break;

            case BS_NAV_RIGHT:  /* page down */
                scroll += visible;
                if (scroll >= count) scroll = count > 0 ? count - 1 : 0;
                dirty = true;
                break;

            default:
                break;
        }
    }
}

const bs_app_t app_log = {
    .name   = "Log",
    .icon   = k_log_icon_16,
    .icon_w = 16,
    .icon_h = 16,
    .run    = app_log_run,
};
