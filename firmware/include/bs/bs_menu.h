#pragma once
#include <stddef.h>
#include "bs_arch.h"
#include "bs_app.h"

typedef enum {
    BS_MENU_AUTO      = 0,   /* auto-select by screen geometry (default) */
    BS_MENU_GRID      = 1,   /* icon grid: big/landscape screens         */
    BS_MENU_SLIDESHOW = 2,   /* left-right carousel with icon + title    */
    BS_MENU_LIST      = 3,   /* vertical list, title only                */
} bs_menu_mode_t;

/* idle_poll: called every iteration of the menu loop - use for konsole_poll(). */
typedef void (*bs_menu_idle_fn)(void);

void                bs_menu_init(const bs_app_t* const* apps, size_t count,
                                 bs_menu_mode_t mode, bs_menu_idle_fn idle_poll);

/* Blocking: draws menu, handles nav, returns selected app (never NULL). */
const bs_app_t*     bs_menu_run(const bs_arch_t* arch);

void                bs_menu_set_mode(bs_menu_mode_t mode);
bs_menu_mode_t      bs_menu_get_mode(void);
void                bs_menu_invalidate(void);   /* force full redraw on next run */
