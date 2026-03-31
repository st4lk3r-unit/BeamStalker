#pragma once
/*
 * bs_demo.h - BeamStalker idle animation: DVD-style BEAMSTALKER bounce.
 *
 * A large "BEAMSTALKER" banner drifts across the display and bounces off
 * edges, cycling through the orange/amber palette on each collision -
 * classic screensaver aesthetic with a Fallout terminal flavour.
 *
 * Call bs_demo_init() once, then bs_demo_tick() every frame.
 * bs_demo_tick() returns true when an exit key is pressed.
 */
#include "bs/bs_arch.h"
#include "bs/bs_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

void bs_demo_init(const bs_arch_t* arch);

/* Returns true if user pressed a key (ESC / Enter / any key) */
bool bs_demo_tick(const bs_arch_t* arch);

#ifdef __cplusplus
}
#endif
