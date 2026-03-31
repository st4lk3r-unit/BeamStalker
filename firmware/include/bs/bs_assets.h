#pragma once
#include <stdint.h>
/* skully    - 120×120 px, 1bpp packed (MSB=left), stride=15 bytes, 1800 bytes total */
/* skull_128 - 128×64  px, 1bpp packed (MSB=left), stride=16 bytes, 1024 bytes total */
/* gear_32   -  32×32  px, 1bpp packed (MSB=left), stride= 4 bytes,  128 bytes total */
/* chart_32  -  32×32  px, 1bpp packed (MSB=left), stride= 4 bytes,  128 bytes total */
extern const uint8_t bs_skull_120[1800];
extern const uint8_t bs_skull_128[1024];
extern const uint8_t bs_gear_32[128];
extern const uint8_t bs_chart_32[128];
