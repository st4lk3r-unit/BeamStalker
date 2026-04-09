/*
 * bs_hw.c - POSIX native hardware info stub.
 */
#include "bs/bs_hw.h"
#include "bs/bs_gfx.h"
#include "board.h"
#include <string.h>

void bs_hw_get_info(bs_hw_info_t* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->board    = BS_BOARD_NAME;
    out->sdk_ver  = "POSIX";
    out->cores    = 1;
    out->chip_rev = -1;
}

int  bs_hw_battery_pct(void) { return 0; }
int  bs_hw_battery_mv(void)  { return 0; }


int  bs_hw_task_list(bs_hw_task_t* tasks, int max_tasks) {
    (void)tasks; (void)max_tasks;
    return -1;
}
