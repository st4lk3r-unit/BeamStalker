#pragma once
#include "bs_keys.h"

typedef enum {
    BS_NAV_NONE = 0,
    BS_NAV_NEXT,     /* right arrow / encoder CW / next in sequence  */
    BS_NAV_PREV,     /* left arrow  / encoder CCW                    */
    BS_NAV_SELECT,   /* Enter / encoder button / confirm             */
    BS_NAV_BACK,     /* Escape / back button / cancel                */
    BS_NAV_UP,
    BS_NAV_DOWN,
    BS_NAV_LEFT,
    BS_NAV_RIGHT,
} bs_nav_id_t;

/* Translate a single key event to a nav action.
 * Returns BS_NAV_NONE for printable chars and other unmapped keys. */
bs_nav_id_t bs_nav_from_key(bs_key_t key);

/* Poll bs_keys and return nav action (BS_NAV_NONE if no event or not nav). */
bs_nav_id_t bs_nav_poll(void);
