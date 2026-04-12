#include "bs/bs_board.h"
#include "board.h"
#include <string.h>

#ifndef BS_BOARD_ARCH_DESC
#  define BS_BOARD_ARCH_DESC "unknown arch"
#endif
#ifndef BS_BOARD_KEYBOARD_DESC
#  define BS_BOARD_KEYBOARD_DESC "not configured"
#endif
#ifndef BS_BOARD_CAP_FLAGS
#  define BS_BOARD_CAP_FLAGS 0u
#endif
#ifndef BS_BOARD_UI_IDLE_DELAY_MS
#  define BS_BOARD_UI_IDLE_DELAY_MS 2
#endif

__attribute__((weak)) int bs_board_platform_init(const bs_arch_t* arch) {
    (void)arch;
    return 0;
}

__attribute__((weak)) void bs_board_platform_prepare_fs_mount(void) {}

__attribute__((weak)) int bs_board_platform_sd_detect(void) {
    return -1;
}

__attribute__((weak)) void bs_board_platform_diag_read(bs_board_diag_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->available = 0;
    out->reachable = 0;
    out->input_1 = out->output_1 = out->config_1 = 0xFF;
}

static const bs_board_desc_t s_desc = {
    .name             = BS_BOARD_NAME,
    .arch_desc        = BS_BOARD_ARCH_DESC,
    .keyboard_desc    = BS_BOARD_KEYBOARD_DESC,
    .caps             = BS_BOARD_CAP_FLAGS,
    .ui_idle_delay_ms = (uint8_t)BS_BOARD_UI_IDLE_DELAY_MS,
};

const bs_board_desc_t* bs_board_desc(void) {
    return &s_desc;
}

uint32_t bs_board_caps(void) {
    return s_desc.caps;
}

int bs_board_ui_idle_delay_ms(void) {
    return (int)(s_desc.ui_idle_delay_ms ? s_desc.ui_idle_delay_ms : 2u);
}

int bs_board_init(const bs_arch_t* arch) {
    return bs_board_platform_init(arch);
}

void bs_board_prepare_fs_mount(void) {
    bs_board_platform_prepare_fs_mount();
}

int bs_board_sd_detect(void) {
    return bs_board_platform_sd_detect();
}

void bs_board_diag_read(bs_board_diag_t* out) {
    bs_board_platform_diag_read(out);
}
