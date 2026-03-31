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

/* Text scale (1=small, 2=medium, 3=large) */
int  bs_ui_text_scale(void);
void bs_ui_set_text_scale(int s);

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
