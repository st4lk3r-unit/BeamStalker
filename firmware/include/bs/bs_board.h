#pragma once

#include <stdint.h>
#include "bs_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BS_BOARD_CAP_FAST_UI      (1u << 0)
#define BS_BOARD_CAP_SIC          (1u << 1)
#define BS_BOARD_CAP_ROTARY       (1u << 2)
#define BS_BOARD_CAP_SINGLE_KEY   (1u << 3)
#define BS_BOARD_CAP_XL9555_DIAG  (1u << 4)
#define BS_BOARD_CAP_SHARED_SD_SPI (1u << 5)

typedef struct {
    const char* name;
    const char* arch_desc;
    const char* keyboard_desc;
    uint32_t    caps;
    uint8_t     ui_idle_delay_ms;
} bs_board_desc_t;

typedef struct {
    int     available;
    int     reachable;
    uint8_t input_1;
    uint8_t output_1;
    uint8_t config_1;
} bs_board_diag_t;

const bs_board_desc_t* bs_board_desc(void);
uint32_t               bs_board_caps(void);
int                    bs_board_ui_idle_delay_ms(void);
int                    bs_board_init(const bs_arch_t* arch);
void                   bs_board_prepare_fs_mount(void);
int                    bs_board_sd_detect(void);
void                   bs_board_diag_read(bs_board_diag_t* out);

#ifdef __cplusplus
}
#endif
