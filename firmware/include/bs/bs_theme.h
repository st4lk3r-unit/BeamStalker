#pragma once
#include "bs_gfx.h"

/*
 * Color palette — the 6 color slots without border style.
 * Combine with a bs_border_style_t to assemble a bs_theme_t.
 */
typedef struct {
    bs_color_t bg;
    bs_color_t primary;
    bs_color_t secondary;
    bs_color_t dim;
    bs_color_t accent;
    bs_color_t warn;
} bs_palette_t;

/* Full theme = palette + border style */
typedef struct {
    bs_color_t        bg;
    bs_color_t        primary;
    bs_color_t        secondary;
    bs_color_t        dim;
    bs_color_t        accent;
    bs_color_t        warn;
    bs_border_style_t border;    /* border drawing style              */
} bs_theme_t;

extern bs_theme_t g_bs_theme;   /* active theme - read/write freely */

extern const bs_theme_t bs_theme_orange;    /* default: Pip-Boy amber        */
extern const bs_theme_t bs_theme_green;     /* classic terminal green        */
extern const bs_theme_t bs_theme_blue;      /* cool blue                     */
extern const bs_theme_t bs_theme_white;     /* high-contrast white           */
extern const bs_theme_t bs_theme_tactical;  /* phosphor green, HUD brackets  */

/* Apply a full theme (copies into g_bs_theme). */
void bs_theme_set(const bs_theme_t* theme);

/*
 * Color palettes — pick independently from border style.
 *   0=Orange  1=Green  2=Blue  3=White  4=Tactical-green
 */
#define BS_PALETTE_COUNT 5
extern const bs_palette_t* const bs_palettes[BS_PALETTE_COUNT];
extern const char* const         bs_palette_names[BS_PALETTE_COUNT];

/* Border styles — pick independently from color palette. */
#define BS_BORDER_STYLE_COUNT 3
extern const char* const bs_border_style_names[BS_BORDER_STYLE_COUNT];

/* Combine palette index + border style into g_bs_theme. */
void bs_theme_apply(int palette_idx, bs_border_style_t border_style);

/* Number of built-in themes (legacy, kept for compatibility). */
#define BS_THEME_COUNT 5
extern const bs_theme_t* const bs_themes[BS_THEME_COUNT];
extern const char* const       bs_theme_names[BS_THEME_COUNT];
