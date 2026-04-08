#pragma once
#include "bs_gfx.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard header/content layout */
int  bs_ui_header_h(void);                   /* header bar height (scales with text_scale) */
void bs_ui_draw_header(const char* title);   /* header bar + separator */
void bs_ui_draw_hint(const char* hint);      /* hint line at bottom (auto-scales, clamps to fit) */
int  bs_ui_content_y(void);                  /* y where content starts */
int  bs_ui_content_h(void);                  /* height of content area */

/* Draw text clipped to a fixed-width box.
 * If text fits in w pixels: drawn left-aligned.
 * If text overflows: animated marquee carousel (requires bs_ui_tick() to advance). */
void bs_ui_draw_text_box(int x, int y, int w, const char* text, bs_color_t col, float scale);

/* Draw up/down scroll caret indicators at the right edge of the content area.
 * Call after drawing list rows.  No-op when all items fit (total <= visible). */
void bs_ui_draw_scroll_arrows(int scroll, int total, int visible);

/* Advance the carousel animation counter — called automatically from bs_gfx_present(). */
void bs_ui_tick(void);

/* Text scale (1.0=small, 1.5=mid-small, 2.0=medium, 2.5=mid-large, 3.0=large) */
float bs_ui_text_scale(void);
void  bs_ui_set_text_scale(float s);

/* Brightness (0-100 %) */
int  bs_ui_brightness(void);
void bs_ui_set_brightness(int pct);

/* Show battery voltage in header (default: off) */
bool bs_ui_show_voltage(void);
void bs_ui_set_show_voltage(bool show);

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
