#pragma once
#include "bs_gfx.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standard header / content layout ───────────────────────────────────── */
int  bs_ui_header_h(void);                   /* header bar height (scales with text_scale) */
void bs_ui_draw_header(const char* title);   /* header bar + separator */
void bs_ui_draw_hint(const char* hint);      /* hint line at bottom (auto-scales, clamps to fit) */
int  bs_ui_content_y(void);                  /* y where content starts (= header height) */
int  bs_ui_content_h(void);                  /* height of usable content area */

/* ── Text box / carousel ─────────────────────────────────────────────────── */
/* Draw text clipped to a fixed-width box.
 * active=false OR carousel disabled → static clip (no animation).
 * active=true  AND carousel enabled → animated marquee. */
void bs_ui_draw_text_box(int x, int y, int w, const char* text,
                          bs_color_t col, float scale, bool active);

bool bs_ui_carousel_enabled(void);
void bs_ui_set_carousel(bool on);

/* Advance the carousel animation by a measured delta in milliseconds.
 * Call once per main-loop iteration: bs_ui_advance_ms(now - prev_ms).
 * bs_ui_tick() is kept as a no-op weak-symbol target for backends. */
void bs_ui_advance_ms(uint32_t ms);
void bs_ui_tick(void);

/* ── Scroll helpers ──────────────────────────────────────────────────────── */
/* Draw up/down scroll caret indicators at the right edge of the content area.
 * No-op when all items fit (total <= visible). */
void bs_ui_draw_scroll_arrows(int scroll, int total, int visible);

/* Clamp *scroll so that cursor stays within the [*scroll, *scroll+visible) window. */
void bs_ui_list_clamp_scroll(int cursor, int* scroll, int total, int visible);

/* ── Row height helpers ──────────────────────────────────────────────────── */
/* Height of a standard single-line row (text + 4 px padding) at scale ts. */
int bs_ui_row_h(float ts);

/* Height of a two-line menu row (name + subtitle + vertical padding). */
int bs_ui_menu_row_h(float ts);

/* How many single-line rows fit in the content area. */
int bs_ui_list_visible(float ts);

/* How many two-line menu rows fit in the content area. */
int bs_ui_menu_visible(float ts);

/* ── Compound row widgets ────────────────────────────────────────────────── */
/*
 * bs_ui_draw_menu_row  — two-line selectable entry (name at ts, desc at ts-0.5).
 * Fills a dim highlight when selected; carousels both lines if they overflow.
 * Returns the row height consumed (= bs_ui_menu_row_h(ts)).
 */
int bs_ui_draw_menu_row(int y, const char* name, const char* desc,
                         bool selected, float ts);

/*
 * bs_ui_draw_kv_row  — key / value info row split at screen mid-point.
 *   key  — left column (dim or secondary)
 *   val  — right column (accent when selected, primary otherwise, warn when warn=true)
 *   warn — draw value in theme.warn colour
 * Returns the row height consumed (= bs_ui_row_h(ts)).
 */
int bs_ui_draw_kv_row(int y, const char* key, const char* val,
                       bool selected, bool warn, float ts);

/* Text scale (1.0=small, 1.5=mid-small, 2.0=medium, 2.5=mid-large, 3.0=large) */
float bs_ui_text_scale(void);
void  bs_ui_set_text_scale(float s);

/* Brightness (0-100 %) */
int  bs_ui_brightness(void);
void bs_ui_set_brightness(int pct);

/* Show battery voltage in header (default: off) */
bool bs_ui_show_voltage(void);
void bs_ui_set_show_voltage(bool show);

/* Header brand mode (0=text title, 1=bitmap logo) */
int  bs_ui_header_brand_mode(void);
void bs_ui_set_header_brand_mode(int mode);

/* Grid max columns (0=auto/ceil(sqrt(n)), 2/3/4=hard limit) */
int  bs_ui_grid_max_cols(void);
void bs_ui_set_grid_max_cols(int n);

/* Grid max rows (0=auto, 1/2/3=hard limit on visible rows) */
int  bs_ui_grid_max_rows(void);
void bs_ui_set_grid_max_rows(int n);

void bs_ui_load_settings(void);  /* reads text_scale + brightness from FS */

#ifdef __cplusplus
}
#endif
