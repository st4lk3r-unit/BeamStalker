#pragma once
/*
 * bs_log.h - BeamStalker firmware logging.
 *
 * Routes to:
 *   - konsole (UART serial) - always, when a konsole is attached
 *   - bs_gfx display        - when a display is available (SGFX / native)
 *
 * BS_LOG_BOOT is a special level for the boot sequence: messages are
 * formatted with a [ OK ] / [WARN] / [FAIL] status tag.
 */
#include <stdarg.h>
#include <stdbool.h>
#include "bs_gfx.h"
#include "konsole/konsole.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BS_LOG_INFO = 0,  /* general informational */
    BS_LOG_WARN,      /* non-fatal warning      */
    BS_LOG_ERR,       /* error                  */
    BS_LOG_BOOT_OK,   /* boot entry: success    */
    BS_LOG_BOOT_WARN, /* boot entry: warning    */
    BS_LOG_BOOT_FAIL, /* boot entry: failure    */
} bs_log_lvl_t;

/* Attach a konsole instance for UART output (may be NULL on native) */
void bs_log_init(struct konsole* ks);

/* Reposition the boot-log Y cursor on the display (called by bs_boot.c) */
void bs_log_boot_reset(int start_y);

/*
 * Print all in-memory log entries (ring buffer, boot to now) to a konsole.
 * The ring is never cleared - call this any time to see the full history.
 */
void bs_log_print_all(struct konsole* ks);

/*
 * Flush the in-memory log ring to the SD filesystem (system.log).
 * Call once after bs_fs_init() succeeds.  Rotates the file when it
 * exceeds 64 kB.  The ring is NOT cleared - bs_log_print_all() still
 * works after this call.  Subsequent log() calls also append to the file.
 */
void bs_log_flush_sd(void);

void bs_log(bs_log_lvl_t lvl, const char* tag, const char* fmt, ...);

/* ---- In-memory ring access (for the log viewer app) ------------------- */

/* Total entries currently in the ring (0 .. LOG_BUF_LINES). */
int bs_log_count(void);

/* Entry at logical index i (0=oldest).  Returns NULL if out of range.
 * The string includes the level tag and trailing '\n'. */
const char* bs_log_entry(int i);

/* Log level of entry i. */
bs_log_lvl_t bs_log_entry_lvl(int i);

/* Theme color corresponding to a log level (uses g_bs_theme). */
bs_color_t bs_log_level_color(bs_log_lvl_t lvl);

/* Convenience macros */
#define BS_LOGI(tag, ...) bs_log(BS_LOG_INFO,      tag, __VA_ARGS__)
#define BS_LOGW(tag, ...) bs_log(BS_LOG_WARN,      tag, __VA_ARGS__)
#define BS_LOGE(tag, ...) bs_log(BS_LOG_ERR,       tag, __VA_ARGS__)
#define BS_LOGOK(tag, ...) bs_log(BS_LOG_BOOT_OK,  tag, __VA_ARGS__)
#define BS_LOGBW(tag, ...) bs_log(BS_LOG_BOOT_WARN,tag, __VA_ARGS__)
#define BS_LOGBF(tag, ...) bs_log(BS_LOG_BOOT_FAIL,tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
