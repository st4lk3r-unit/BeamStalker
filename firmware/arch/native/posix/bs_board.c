#include "bs/bs_board.h"
#include <string.h>

int bs_board_platform_init(const bs_arch_t* arch) {
    (void)arch;
    return 0;
}

void bs_board_platform_prepare_fs_mount(void) {}

int bs_board_platform_sd_detect(void) {
    return -1;
}

void bs_board_platform_diag_read(bs_board_diag_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->available = 0;
    out->reachable = 0;
    out->input_1 = out->output_1 = out->config_1 = 0xFF;
}
