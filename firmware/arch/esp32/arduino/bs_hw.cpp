/*
 * bs_hw.cpp - ESP32/Arduino hardware info implementation.
 *
 * C++ file so we can use the Arduino ESP class for flash/sketch size.
 * All symbols exported as extern "C" for C callers.
 */
#ifdef ARCH_ESP32

extern "C" {
#include "bs/bs_hw.h"
#include "board.h"
}

#ifdef BS_USE_SIC
extern "C" {
#  include <sic/power/battery.h>
#  include <sic/bus/i2c_bus.h>
}
#endif

#include <Esp.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_chip_info.h>
#include <esp32-hal-cpu.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

extern "C" void bs_hw_get_info(bs_hw_info_t* out) {
    if (!out) return;

    out->board            = BS_BOARD_NAME;
    out->cpu_mhz          = getCpuFrequencyMhz();
    out->flash_kb         = (uint32_t)(ESP.getFlashChipSize() / 1024u);
    out->heap_free_kb     = esp_get_free_heap_size()         / 1024u;
    out->heap_min_kb      = esp_get_minimum_free_heap_size() / 1024u;
    out->heap_total_kb    = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024u);
    out->heap_internal_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)  / 1024u);
    out->psram_free_kb    = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)    / 1024u);
    out->psram_total_kb   = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)   / 1024u);
    out->firmware_kb      = (uint32_t)(ESP.getSketchSize()      / 1024u);
    out->flash_free_kb    = (uint32_t)(ESP.getFreeSketchSpace() / 1024u);
    out->sdk_ver          = esp_get_idf_version();
    out->task_count       = (uint32_t)uxTaskGetNumberOfTasks();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    out->chip_rev = (int)chip.revision;
    out->cores    = (int)chip.cores;
}

extern "C" int bs_hw_battery_pct(void) {
#ifdef BS_USE_SIC
    sic_battery_t bat;
    if (sic_battery_read(&bat) != 0) return 0;

    /*
     * BQ27220 SOC register often reads 100% before the gauge has completed
     * a learning cycle. Fall back to a simple linear LiPo estimate when
     * SOC ≥99% but voltage < 4.05V.
     */
    if (bat.percent >= 99 && bat.voltage_v > 1.0f && bat.voltage_v < 4.05f) {
        float pct = (bat.voltage_v - 3.0f) / 1.2f * 100.0f;
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        return (int)pct;
    }
    return bat.percent > 100 ? 100 : bat.percent;
#endif
    return 0;
}

extern "C" int bs_hw_battery_mv(void) {
#ifdef BS_USE_SIC
    sic_battery_t bat;
    if (sic_battery_read(&bat) == 0 && bat.voltage_v > 0.0f)
        return (int)(bat.voltage_v * 1000.0f);
#endif
    return 0;
}

/* ---- XL9555 GPIO expander diagnostic ----------------------------------- */

extern "C" void bs_hw_xl9555_read(bs_hw_xl9555_t* out) {
    if (!out) return;
    out->reachable = 0;
    out->input_1 = out->output_1 = out->config_1 = 0xFF;
#ifdef BS_USE_SIC
    uint8_t reg;
    reg = 0x01; if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->input_1,  1) < 0) return;
    reg = 0x03; if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->output_1, 1) < 0) return;
    reg = 0x07; if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->config_1, 1) < 0) return;
    out->reachable = 1;
#endif
}

extern "C" int bs_hw_sd_detect(void) {
#ifdef BS_USE_SIC
    uint8_t reg = 0x01, val = 0xFF;
    if (sic_i2c_writeread(0, 0x20, &reg, 1, &val, 1) < 0) return -1;
    return (val & (1 << 2)) ? 0 : 1;  /* P12=SD_DET: LOW=card, HIGH=no card */
#endif
    return -1;
}

/* ---- FreeRTOS task list ------------------------------------------------- */

extern "C" int bs_hw_task_list(bs_hw_task_t* tasks, int max_tasks) {
#if configUSE_TRACE_FACILITY
    if (!tasks || max_tasks <= 0) return 0;

    TaskStatus_t* buf = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * (size_t)max_tasks);
    if (!buf) return -1;

    uint32_t total_rt = 0;
    int n = (int)uxTaskGetSystemState(buf, (UBaseType_t)max_tasks, &total_rt);
    if (n > max_tasks) n = max_tasks;

    for (int i = 0; i < n; i++) {
        strncpy(tasks[i].name, buf[i].pcTaskName, 15);
        tasks[i].name[15] = '\0';
        /* usStackHighWaterMark is in words; StackType_t is 4 bytes on ESP32 */
        tasks[i].stack_hwm_b = (uint32_t)buf[i].usStackHighWaterMark * sizeof(StackType_t);
        tasks[i].priority    = (uint32_t)buf[i].uxCurrentPriority;
    }

    free(buf);
    return n;
#else
    (void)tasks; (void)max_tasks;
    return -1;  /* configUSE_TRACE_FACILITY not enabled */
#endif
}

#endif /* ARCH_ESP32 */
