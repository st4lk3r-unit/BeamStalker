/*
 * app_dvd.c - DVD bounce animation, wrapped as a bs_app.
 */
#include "apps/app_dvd.h"
#include "bs_demo.h"
#include "bs/bs_gfx.h"
#include "bs/bs_debug.h"
#include "bs/bs_assets.h"

static void dvd_run(const bs_arch_t* arch) {
    bs_demo_init(arch);
    while (!bs_demo_tick(arch)) {
        bs_debug_frame();
        bs_gfx_present();
    }
}

const bs_app_t app_dvd = {
    .name   = "DVD",
    .icon   = bs_skull_120,
    .icon_w = 120,
    .icon_h = 120,
    .run    = dvd_run,
};
