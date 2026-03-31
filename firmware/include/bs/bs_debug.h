#pragma once
/*
 * bs_debug.h - Debug overlay: FPS counter + hardware stats.
 *
 * Call bs_debug_frame() every frame BEFORE bs_gfx_present().
 * When enabled, draws a semi-transparent overlay in the top-right corner
 * showing FPS, heap, PSRAM, and CPU info.  Stats refresh every 2 s.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "bs/bs_arch.h"
#include <stdbool.h>

void bs_debug_init(const bs_arch_t* arch);
void bs_debug_set_enabled(bool en);
bool bs_debug_is_enabled(void);
void bs_debug_frame(void);   /* call each frame before bs_gfx_present() */

#ifdef __cplusplus
}
#endif
