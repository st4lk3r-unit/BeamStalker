#pragma once
#include "bs_arch.h"

/* An app descriptor - statically declared, const, zero-init safe. */
typedef struct bs_app {
    const char*    name;      /* display name (keep short, ≤12 chars)   */
    const uint8_t* icon;      /* 1bpp packed icon bitmap, or NULL        */
    int            icon_w;    /* icon source width  (0 if icon==NULL)    */
    int            icon_h;    /* icon source height (0 if icon==NULL)    */
    /* Blocking run function. Returns when the app exits (user pressed back). */
    void (*run)(const bs_arch_t* arch);
} bs_app_t;
