/*
 * bs_menu.c - BeamStalker menu engine.
 *
 * Layout auto-selection heuristic:
 *   w >= 240 && h >= 160  → GRID      (icon grid, big/landscape)
 *   w > h                 → SLIDESHOW  (left-right carousel, small landscape)
 *   else                  → LIST       (vertical list, portrait/narrow)
 *
 * GRID:
 *   - Header bar (16px): "BeamStalker" left
 *   - Separator (1px) at y=15
 *   - Cards arranged in a grid; up to 4 cols on wide screens
 *   - Each card: border lines (dim=unselected, accent=selected)
 *   - Icon centered in card (bitmap scaled to fit, or placeholder box)
 *   - App name centered below icon (scale=1)
 *
 * SLIDESHOW:
 *   - Header bar (16px)
 *   - One app centered, large icon
 *   - Name below icon (scale=2)
 *   - "<" on left edge, ">" on right edge
 *   - Page indicator "N / Total" at bottom center
 *
 * LIST:
 *   - Header bar (16px)
 *   - Each row 14px: name text, selected row accent bg
 *   - Scrolls if needed
 *
 * Navigation (all modes):
 *   NEXT / RIGHT    → next item (wrap)
 *   PREV / LEFT     → prev item (wrap)
 *   DOWN            → next row (GRID) or next item
 *   UP              → prev row (GRID) or prev item
 *   SELECT          → launch selected app → return from bs_menu_run()
 *   BACK            → no-op at top-level menu
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_app.h"
#include "bs/bs_menu.h"
#include "bs/bs_gfx.h"
#include "bs/bs_assets.h"
#include "bs/bs_debug.h"
#include "bs/bs_ui.h"
#include "bs/bs_hw.h"
#include "bs/bs_fs.h"
#include <stdio.h>

/* ---- State ------------------------------------------------------------ */
static const bs_app_t* const* s_apps;
static size_t           s_count;
static size_t           s_cursor;
static bs_menu_mode_t   s_mode;
static bs_menu_idle_fn  s_idle;
static bool             s_dirty;
static int              s_grid_scroll;  /* first visible row in grid mode */

/* ---- Helpers ---------------------------------------------------------- */

static bs_menu_mode_t actual_mode(void) {
    if (s_mode != BS_MENU_AUTO) return s_mode;
    int sw = bs_gfx_width(), sh = bs_gfx_height();
    if (sw >= 240 && sh >= 160) return BS_MENU_GRID;
    if (sw > sh) return BS_MENU_SLIDESHOW;
    return BS_MENU_LIST;
}

/* Integer ceil(sqrt(n)). */
static int isqrt_ceil(int n) {
    int r = 1;
    while (r * r < n) r++;
    return r;
}

static int grid_cols(void) {
    int n   = (int)s_count;
    int max = bs_ui_grid_max_cols();  /* 0=auto */

    int c;
    if (max == 0) {
        /* Auto: pick cols so the grid is as close to square as possible */
        c = isqrt_ceil(n);
    } else {
        c = max;
    }
    if (c > 4) c = 4;
    if (c < 1) c = 1;
    if (c > n) c = n;
    return c;
}

/* ---- Status icons (header right side) --------------------------------- */

/* Battery body outline + fill, at (bx, by), body w=ih*2, h=ih. */
static void draw_bat_icon(int bx, int by, int ih, int pct, bs_color_t col) {
    int bw = ih * 2;
    int nub = ih / 3; if (nub < 1) nub = 1;
    bs_gfx_hline(bx, by, bw, col);
    bs_gfx_hline(bx, by+ih-1, bw, col);
    bs_gfx_fill_rect(bx, by, 1, ih, col);
    bs_gfx_fill_rect(bx+bw-1, by, 1, ih, col);
    bs_gfx_fill_rect(bx+bw, by+(ih-nub)/2, 2, nub, col);
    if (pct > 0) {
        int fw = ((bw - 4) * pct) / 100;
        if (fw < 1) fw = 1;
        bs_gfx_fill_rect(bx+2, by+2, fw, ih-4, col);
    }
}

/* SD card silhouette - solid filled portrait card with top-left notch. */
static void draw_sd_icon(int sx, int sy, int ih, bs_color_t col) {
    int sw2 = (ih * 2 + 2) / 3; if (sw2 < 5) sw2 = 5;
    int notch = sw2 / 3; if (notch < 1) notch = 1;
    /* Fill body below the notch line */
    bs_gfx_fill_rect(sx, sy + notch, sw2, ih - notch, col);
    /* Fill top rows with diagonal left edge */
    for (int row = 0; row < notch; row++) {
        int lx = sx + notch - row;
        int w  = sw2 - (notch - row);
        if (w > 0) bs_gfx_fill_rect(lx, sy + row, w, 1, col);
    }
}

/* SD card outline — perimeter only, for "expected but not mounted" state. */
static void draw_sd_icon_outline(int sx, int sy, int ih, bs_color_t col) {
    int sw2   = (ih * 2 + 2) / 3; if (sw2 < 5) sw2 = 5;
    int notch = sw2 / 3; if (notch < 1) notch = 1;
    /* Top edge (right of chamfer start) */
    bs_gfx_hline(sx + notch, sy, sw2 - notch, col);
    /* Chamfer diagonal: rows 1..notch-1 (row 0 already covered by top hline) */
    for (int r = 1; r < notch; r++)
        bs_gfx_fill_rect(sx + notch - r, sy + r, 1, 1, col);
    /* Left edge (below chamfer) */
    bs_gfx_fill_rect(sx, sy + notch, 1, ih - notch, col);
    /* Right edge */
    bs_gfx_fill_rect(sx + sw2 - 1, sy, 1, ih, col);
    /* Bottom edge */
    bs_gfx_hline(sx, sy + ih - 1, sw2, col);
}

/* ---- Header ----------------------------------------------------------- */

static int s_bat_pct    = 0;
static int s_bat_mv     = 0;
static int s_bat_frames = 0;

static void draw_header(void) {
    int sw  = bs_gfx_width();
    float ts = bs_ui_text_scale();
    int hh  = bs_ui_header_h();
    int ty  = (hh - bs_gfx_text_h(ts)) / 2;
    int ih  = bs_gfx_text_h(ts);

    bs_gfx_fill_rect(0, 0, sw, hh, g_bs_theme.bg);

    /* --- Right side status icons (draw right-to-left) --- */
    int rx = sw - 4;
    int iy = (hh - ih) / 2;  /* vertical centre of icon = text baseline */

    /* Battery */
    if (--s_bat_frames <= 0) {
        s_bat_pct    = bs_hw_battery_pct();
        s_bat_mv     = bs_hw_battery_mv();
        s_bat_frames = 150;   /* refresh ~every 150 menu frames */
    }
    if (s_bat_pct > 0) {
        bs_color_t bat_col = (s_bat_pct < 20) ? g_bs_theme.warn : g_bs_theme.dim;
        char pct_buf[20];
        if (bs_ui_show_voltage() && s_bat_mv > 0)
            snprintf(pct_buf, sizeof pct_buf, "%d%%  %d.%02dV",
                     s_bat_pct, s_bat_mv / 1000, (s_bat_mv % 1000) / 10);
        else
            snprintf(pct_buf, sizeof pct_buf, "%d%%", s_bat_pct);
        int tw = bs_gfx_text_w(pct_buf, ts);
        rx -= tw;
        bs_gfx_text(rx, iy, pct_buf, bat_col, ts);
        rx -= 3;
        int icon_w = ih * 2 + 2;  /* body + nub */
        rx -= icon_w;
        draw_bat_icon(rx, iy, ih, s_bat_pct, bat_col);
        rx -= 6;
    }

    /* SD card status icon:
     *   mounted           → filled silhouette (dim)
     *   expected but fail → outline only (warn)
     *   not configured    → hidden                */
    {
        int sd_w = (ih * 2 + 2) / 3 + 2; if (sd_w < 6) sd_w = 6;
        if (bs_fs_available()) {
            rx -= sd_w;
            draw_sd_icon(rx, iy, ih, g_bs_theme.dim);
            rx -= 4;
        } else if (bs_fs_init_error() != NULL) {
            rx -= sd_w;
            draw_sd_icon_outline(rx, iy, ih, g_bs_theme.warn);
            rx -= 4;
        }
    }

    /* --- Left side title / logo --- */
    if (bs_ui_header_brand_mode() != 0) {
        int max_w = rx - 12;
        int max_h = hh - 4;
        if (max_w > 8 && max_h > 4) {
            int step_w = (120 + max_w - 1) / max_w;
            int step_h = (120 + max_h - 1) / max_h;
            int step   = step_w > step_h ? step_w : step_h;
            if (step < 1) step = 1;
            int out_h = (120 + step - 1) / step;
            int ox = 8;
            int oy = (hh - out_h) / 2;
            bs_gfx_bitmap_1bpp(ox, oy, 120, 120, bs_skull_120, g_bs_theme.dim, 1, step);
        }
    } else {
        float title_ts = ts;
        while (title_ts > 1.0f && 8 + bs_gfx_text_w("BeamStalker", title_ts) > rx - 8)
            title_ts -= 0.5f;
        bs_gfx_text(8, (hh - bs_gfx_text_h(title_ts)) / 2, "BeamStalker",
                    g_bs_theme.dim, title_ts);
    }

    bs_gfx_hline(0, hh - 1, sw, g_bs_theme.dim);
}

/* ---- GRID ------------------------------------------------------------- */

static void draw_grid(void) {
    int sw  = bs_gfx_width();
    int sh  = bs_gfx_height();
    int hh  = bs_ui_header_h();
    float ns = bs_ui_text_scale(); if (ns > 2.0f) ns = 2.0f;
    int nh  = bs_gfx_text_h(ns);

    int cols       = grid_cols();
    int total_rows = ((int)s_count + cols - 1) / cols;

    /* Visible rows: how many fit with at least 50px cell height,
     * capped by bs_ui_grid_max_rows() when non-zero.            */
    int max_vis = (sh - hh) / 50;
    if (max_vis < 1) max_vis = 1;
    int mr = bs_ui_grid_max_rows();
    if (mr > 0 && mr < max_vis) max_vis = mr;
    int vis_rows = total_rows < max_vis ? total_rows : max_vis;
    int cell_w   = sw / cols;
    int cell_h   = (sh - hh) / vis_rows;

    /* Keep cursor row in view */
    int cursor_row = (int)s_cursor / cols;
    if (cursor_row < s_grid_scroll)              s_grid_scroll = cursor_row;
    if (cursor_row >= s_grid_scroll + vis_rows)  s_grid_scroll = cursor_row - vis_rows + 1;
    if (s_grid_scroll < 0)                       s_grid_scroll = 0;

    for (int i = 0; i < (int)s_count; i++) {
        int row = i / cols;
        if (row < s_grid_scroll || row >= s_grid_scroll + vis_rows) continue;

        const bs_app_t* app = s_apps[i];
        int col  = i % cols;
        int disp = row - s_grid_scroll;  /* display row on screen */

        int card_x = col * cell_w + 4;
        int card_y = hh + disp * cell_h + 4;
        int card_w = cell_w - 8;
        int card_h = cell_h - 8;

        bool selected = ((size_t)i == s_cursor);

        /* FILL: dim fill + primary icons (top-bar look).
         * SHARP/BRACKET: accent outline + primary icons. */
        bs_color_t border_col = selected
            ? (g_bs_theme.border == BS_BORDER_FILL ? g_bs_theme.dim : g_bs_theme.accent)
            : g_bs_theme.dim;
        bs_color_t icon_col = selected ? g_bs_theme.primary : g_bs_theme.secondary;
        bs_color_t name_col = icon_col;

        /* Border/fill: unselected cards only get a border in SHARP style */
        if (selected) {
            bs_gfx_border(card_x, card_y, card_w, card_h, border_col, g_bs_theme.border);
        } else if (g_bs_theme.border == BS_BORDER_SHARP) {
            bs_gfx_border(card_x, card_y, card_w, card_h, border_col, BS_BORDER_SHARP);
        }

        /* Icon area: fills the card height minus name label + padding.
         * Icon is scaled to fill and centered both horizontally and vertically. */
        int icon_area_h = card_h - nh - 12;
        int icon_area_w = card_w - 8;
        if (icon_area_h < 4) icon_area_h = 4;
        if (icon_area_w < 4) icon_area_w = 4;

        int icon_top = card_y + 4;  /* top of icon area */

        if (app->icon != NULL) {
            int tgt = icon_area_h < icon_area_w ? icon_area_h : icon_area_w;
            int src_max = app->icon_w > app->icon_h ? app->icon_w : app->icon_h;
            int step = (src_max + tgt - 1) / tgt;
            if (step < 1) step = 1;
            int rw = (app->icon_w + step - 1) / step;
            int rh = (app->icon_h + step - 1) / step;
            int rmax = rw > rh ? rw : rh;
            int scale = icon_area_h / (rmax > 0 ? rmax : 1);
            int scale_w = icon_area_w / (rw > 0 ? rw : 1);
            if (scale_w < scale) scale = scale_w;
            if (scale < 1) scale = 1;
            int ox = card_x + (card_w - rw * scale) / 2;
            int oy = icon_top + (icon_area_h - rh * scale) / 2;
            bs_gfx_bitmap_1bpp(ox, oy, app->icon_w, app->icon_h,
                               app->icon, icon_col, scale, step);
        } else {
            int ps = icon_area_h / 2; if (ps < 4) ps = 4;
            int px = card_x + (card_w - ps) / 2;
            int py = icon_top + (icon_area_h - ps) / 2;
            bs_gfx_border(px, py, ps, ps, border_col, BS_BORDER_SHARP);
        }

        /* Name at the bottom of the card.
         * Center it when it fits; fall back to carousel clipping when it does not. */
        int name_y = card_y + card_h - nh - 4;
        int name_w = card_w - 4;
        int text_w = bs_gfx_text_w(app->name, ns);
        if (text_w <= name_w) {
            int name_x = card_x + (card_w - text_w) / 2;
            bs_gfx_text(name_x, name_y, app->name, name_col, ns);
        } else {
            bs_ui_draw_text_box(card_x + 2, name_y, name_w, app->name, name_col, ns, selected);
        }
    }

    /* Row scroll indicator: tiny dots bottom-right when rows overflow */
    if (total_rows > vis_rows) {
        int dot_x = sw - 6;
        int dot_h = (sh - hh) / total_rows;
        if (dot_h < 2) dot_h = 2;
        for (int r = 0; r < total_rows; r++) {
            bs_color_t dc = (r >= s_grid_scroll && r < s_grid_scroll + vis_rows)
                            ? g_bs_theme.primary : g_bs_theme.dim;
            int dot_y = hh + r * dot_h;
            bs_gfx_fill_rect(dot_x, dot_y, 3, dot_h - 1, dc);
        }
    }
}

/* ---- SLIDESHOW -------------------------------------------------------- */

static void draw_slideshow(void) {
    int sw  = bs_gfx_width();
    int sh  = bs_gfx_height();
    int hh  = bs_ui_header_h();
    float ts = bs_ui_text_scale();
    int name_spare  = bs_gfx_text_h(ts) + 8;
    int page_ind_h  = bs_gfx_text_h(ts) + 6;  /* "N / Total" indicator at bottom */

    const bs_app_t* app = s_apps[s_cursor];

    /* Reserve space for name label AND page indicator so they never overlap.
     * Hard cap at 64px: prevents tiny icons (16×16) scaling to ridiculous sizes. */
    int target = sh - hh - name_spare - page_ind_h - 4;
    if (target > 64) target = 64;
    if (target < 8)  target = 8;

    if (app->icon != NULL) {
        int src_max = app->icon_w > app->icon_h ? app->icon_w : app->icon_h;
        int step = (src_max + target - 1) / target;
        if (step < 1) step = 1;
        int rendered_w = (app->icon_w + step - 1) / step;
        int rendered_h = (app->icon_h + step - 1) / step;
        int rmax = rendered_w > rendered_h ? rendered_w : rendered_h;
        int scale = target / rmax;
        if (scale < 1) scale = 1;
        int ox = (sw - rendered_w * scale) / 2;
        int oy = hh + (sh - hh - rendered_h * scale - name_spare) / 2;
        if (oy < hh) oy = hh;
        bs_gfx_bitmap_1bpp(ox, oy, app->icon_w, app->icon_h,
                           app->icon, g_bs_theme.primary, scale, step);

        /* Name below icon */
        int name_y = oy + rendered_h * scale + 6;
        int nbox_w = sw - 16;
        bs_ui_draw_text_box(8, name_y, nbox_w, app->name, g_bs_theme.primary, ts, true);
    } else {
        /* Placeholder box */
        int box = target;
        int ox = (sw - box) / 2;
        int oy = hh + (sh - hh - box - name_spare) / 2;
        if (oy < hh) oy = hh;
        bs_gfx_border(ox, oy, box, box, g_bs_theme.dim, g_bs_theme.border);

        int name_y = oy + box + 6;
        int nbox_w = sw - 16;
        bs_ui_draw_text_box(8, name_y, nbox_w, app->name, g_bs_theme.primary, ts, true);
    }

    /* Left / right arrows */
    bs_color_t left_col  = (s_cursor > 0) ? g_bs_theme.primary : g_bs_theme.dim;
    bs_color_t right_col = (s_cursor + 1 < s_count) ? g_bs_theme.primary : g_bs_theme.dim;
    int mid_y = hh + (sh - hh) / 2 - bs_gfx_text_h(ts) / 2;
    bs_gfx_text(4, mid_y, "<", left_col, ts);
    bs_gfx_text(sw - bs_gfx_text_w(">", ts) - 4, mid_y, ">", right_col, ts);

    /* Page indicator — bottom-right corner */
    char page_buf[16];
    snprintf(page_buf, sizeof page_buf, "%d/%d", (int)s_cursor + 1, (int)s_count);
    int pw = bs_gfx_text_w(page_buf, ts);
    bs_gfx_text(sw - pw - 4, sh - bs_gfx_text_h(ts) - 3, page_buf, g_bs_theme.dim, ts);
}

/* ---- LIST ------------------------------------------------------------- */

static void draw_list(void) {
    int sw = bs_gfx_width();
    int sh = bs_gfx_height();

    float ts      = bs_ui_text_scale();
    int row_h     = bs_gfx_text_h(ts) + 4;
    int start_y   = bs_ui_header_h();
    int visible   = (sh - start_y) / row_h;

    /* Scroll offset: keep selected visible */
    int scroll = 0;
    if ((int)s_cursor >= visible) {
        scroll = (int)s_cursor - visible + 1;
    }

    for (int v = 0; v < visible; v++) {
        int i = v + scroll;
        if (i >= (int)s_count) break;

        const bs_app_t* app = s_apps[i];
        int row_y = start_y + v * row_h;
        bool selected = ((size_t)i == s_cursor);

        if (selected) {
            bs_gfx_fill_rect(0, row_y, sw, row_h, g_bs_theme.dim);
        }

        bs_color_t tc = selected ? g_bs_theme.primary : g_bs_theme.secondary;
        bs_ui_draw_text_box(6, row_y + 2, sw - 12, app->name, tc, ts, selected);
    }
}

/* ---- Draw dispatcher -------------------------------------------------- */

static void draw_menu_by_mode(void) {
    draw_header();
    switch (actual_mode()) {
        case BS_MENU_GRID:      draw_grid();      break;
        case BS_MENU_SLIDESHOW: draw_slideshow(); break;
        case BS_MENU_LIST:      draw_list();      break;
        default:                draw_grid();      break;
    }
}

/* ---- Public API ------------------------------------------------------- */

void bs_menu_init(const bs_app_t* const* apps, size_t count,
                  bs_menu_mode_t mode, bs_menu_idle_fn idle_poll) {
    s_apps   = apps;
    s_count  = count;
    s_cursor = 0;
    s_mode   = mode;
    s_idle   = idle_poll;
    s_dirty  = true;
}

const bs_app_t* bs_menu_run(const bs_arch_t* arch) {
    s_dirty = true;
    s_grid_scroll = 0;

    uint32_t prev_ms      = arch->millis();
    uint32_t last_anim_ms = prev_ms;

    while (true) {
        uint32_t now = arch->millis();
        uint32_t dt  = now - prev_ms;
        prev_ms = now;
        bs_ui_advance_ms(dt);

        if (s_idle) s_idle();

        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (nav) {
                case BS_NAV_NEXT:
                case BS_NAV_RIGHT:
                    s_cursor = (s_cursor + 1) % s_count;
                    s_dirty = true;
                    break;
                case BS_NAV_PREV:
                case BS_NAV_LEFT:
                    s_cursor = (s_cursor + s_count - 1) % s_count;
                    s_dirty = true;
                    break;
                case BS_NAV_DOWN:
                    /* Step by 1 in all modes: the encoder knob produces UP/DOWN and
                     * must be able to reach every item, including columns > 0. */
                    s_cursor = (s_cursor + 1) % s_count;
                    s_dirty = true;
                    break;
                case BS_NAV_UP:
                    s_cursor = (s_cursor + s_count - 1) % s_count;
                    s_dirty = true;
                    break;
                case BS_NAV_SELECT: {
                    /* Wait for encoder/button release, flush stale events */
                    arch->delay_ms(120);
                    { bs_key_t _k; while (bs_keys_poll(&_k)) {} }
                    return s_apps[s_cursor];
                }
                default:
                    break;
            }
        }

        /* Full-screen redraws on the T-Pager are expensive enough to make the
         * rotary encoder feel lossy if we do them every loop.  Redraw on input,
         * and only refresh periodically to advance any marquee/carousel text. */
        bool anim_due = bs_ui_carousel_enabled() && (uint32_t)(now - last_anim_ms) >= 100U;
        if (s_dirty || anim_due) {
            bs_gfx_clear(g_bs_theme.bg);
            draw_menu_by_mode();
            bs_debug_frame();
            bs_gfx_present();
            s_dirty = false;
            if (anim_due) last_anim_ms = now;
        }

#if defined(VARIANT_TPAGER) || defined(VARIANT_TDONGLE_S3)
        arch->delay_ms(1);
#else
        arch->delay_ms(2);
#endif
    }
}

void bs_menu_set_mode(bs_menu_mode_t mode) {
    s_mode  = mode;
    s_dirty = true;
}

bs_menu_mode_t bs_menu_get_mode(void) {
    return s_mode;
}

void bs_menu_invalidate(void) {
    s_dirty = true;
}
