#pragma once
/*
 * bs_hw.h - Platform hardware info, C interface.
 *
 * Implemented per-arch:
 *   arch/esp32/arduino/bs_hw.cpp  - Arduino/ESP-IDF (C++ wrapper with C linkage)
 *   arch/native/posix/bs_hw.c     - POSIX stub
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    const char* board;           /* board name string              */
    uint32_t    cpu_mhz;         /* CPU frequency MHz (0=unknown)  */
    uint32_t    flash_kb;        /* flash chip size KB             */
    uint32_t    heap_free_kb;    /* free heap KB    (0=unknown)    */
    uint32_t    heap_min_kb;     /* min-ever free heap KB          */
    uint32_t    heap_total_kb;   /* total internal heap KB         */
    uint32_t    heap_internal_kb;/* free internal SRAM-only heap KB*/
    uint32_t    psram_free_kb;   /* free PSRAM KB   (0=none)       */
    uint32_t    psram_total_kb;  /* total PSRAM KB  (0=none)       */
    uint32_t    firmware_kb;     /* compiled sketch/firmware size KB*/
    uint32_t    flash_free_kb;   /* remaining flash for OTA/data KB */
    const char* sdk_ver;         /* SDK/IDF version string         */
    int         chip_rev;        /* chip silicon revision          */
    int         cores;           /* number of CPU cores            */
    uint32_t    task_count;      /* total FreeRTOS task count (0=unknown) */
} bs_hw_info_t;

void bs_hw_get_info(bs_hw_info_t* out);

/* Returns battery percentage 1-100, or 0 if unknown / no battery hardware. */
int  bs_hw_battery_pct(void);
/* Returns battery voltage in mV (e.g. 3800), or 0 if unknown. */
int  bs_hw_battery_mv(void);

/*
 * FreeRTOS task snapshot.
 *   stack_hwm_b : stack headroom remaining in bytes (high-water mark).
 *                 Lower = closer to overflow.
 *   priority    : current task priority.
 */
#define BS_HW_MAX_TASKS 24

typedef struct {
    char     name[16];
    uint32_t stack_hwm_b;
    uint32_t priority;
} bs_hw_task_t;

/*
 * Fill up to max_tasks entries into tasks[].
 * Returns number filled, or -1 if unsupported on this platform.
 * Tasks are returned in unspecified order.
 */
int bs_hw_task_list(bs_hw_task_t* tasks, int max_tasks);

#ifdef __cplusplus
}
#endif
