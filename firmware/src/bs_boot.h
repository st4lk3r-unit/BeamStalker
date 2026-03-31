#pragma once
/*
 * bs_boot.h - BeamStalker boot sequence.
 *
 * Displays an elegant Fallout-flavored initialization sequence:
 *   1. BEAMSTALKER ASCII banner (scaled glyph rendering)
 *   2. Separator line
 *   3. Component init entries, each logged with [ OK ] / [ WARN ] / [ FAIL ]
 *      and a brief timing delay for the "hardware coming up" feel
 *
 * Call bs_boot_run() once at startup after all subsystems are initialised.
 * It drives bs_gfx and bs_log - both must be ready before calling this.
 */
#include "bs/bs_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* idle_fn: called each iteration of the "press any key" wait loop.
 * Use to keep the terminal konsole responsive during the UI wait. */
void bs_boot_run(const bs_arch_t* arch, void (*idle_fn)(void));

#ifdef __cplusplus
}
#endif
