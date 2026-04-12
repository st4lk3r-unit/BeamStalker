/*
 * bs_log.c - BeamStalker logging implementation.
 *
 * Routes to two channels depending on compile target:
 *
 *   Hardware (BS_USE_SGFX / BS_KEYS_SIC):
 *     → konsole (UART serial) with ANSI color codes
 *     → bs_gfx display for BOOT_* levels (rendered below boot header)
 *
 *   Native Linux (BS_GFX_NATIVE):
 *     → konsole not used for stdout (terminal taken by gfx)
 *     → stderr for raw debug messages
 *     → boot log rendered on-screen via bs_gfx_text()
 *
 * Boot display: always rendered at scale=1 (small) for maximum info density.
 * When entries overflow the screen the view auto-scrolls to keep the latest
 * line visible.  Call bs_log_boot_reset() before starting the boot sequence
 * to set the start Y (below the boot header/banner).
 */
#include "bs/bs_log.h"
#include "bs/bs_gfx.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_fs.h"
#include "beamstalker.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---- State ------------------------------------------------------------ */
static struct konsole* s_ks       = NULL;
static int             s_boot_y   = 0;  /* current Y cursor for boot display */
static int             s_boot_start_y = 0;  /* top of boot log area         */

/* ---- File logging ring buffer ----------------------------------------- */
#define LOG_BUF_LINES   128
#define LOG_LINE_MAX    128
#define LOG_FILE        BS_PATH_LOG
#define LOG_MAX_BYTES   (64 * 1024)

static char          s_ring    [LOG_BUF_LINES][LOG_LINE_MAX];
static bs_log_lvl_t  s_ring_lvl[LOG_BUF_LINES];
static int           s_ring_head  = 0;
static int           s_ring_count = 0;
static bool          s_fs_ready   = false;

/* Boot log Y start: set below the header/banner (updated by bs_log_boot_reset) */
#define BOOT_LOG_START_Y_FRAC  42  /* percent of screen height (fallback) */

/* ---- Internal --------------------------------------------------------- */

static const char* level_ansi(bs_log_lvl_t lvl) {
    switch (lvl) {
        case BS_LOG_BOOT_OK:   return "\033[38;2;255;153;0m";
        case BS_LOG_BOOT_WARN: return "\033[38;2;255;48;0m";
        case BS_LOG_BOOT_FAIL: return "\033[38;2;255;30;0m";
        case BS_LOG_WARN:      return "\033[38;2;255;48;0m";
        case BS_LOG_ERR:       return "\033[38;2;255;30;0m";
        default:               return "\033[38;2;255;119;0m";
    }
}

static const char* level_tag(bs_log_lvl_t lvl) {
    switch (lvl) {
        case BS_LOG_BOOT_OK:   return "[  OK  ]";
        case BS_LOG_BOOT_WARN: return "[ WARN ]";
        case BS_LOG_BOOT_FAIL: return "[ FAIL ]";
        case BS_LOG_WARN:      return "[ WARN ]";
        case BS_LOG_ERR:       return "[ ERR  ]";
        default:               return "[  --  ]";
    }
}

bs_color_t bs_log_level_color(bs_log_lvl_t lvl) {
    switch (lvl) {
        case BS_LOG_BOOT_OK:   return g_bs_theme.accent;
        case BS_LOG_BOOT_WARN: return g_bs_theme.warn;
        case BS_LOG_BOOT_FAIL: return g_bs_theme.warn;
        case BS_LOG_WARN:      return g_bs_theme.warn;
        case BS_LOG_ERR:       return g_bs_theme.warn;
        default:               return g_bs_theme.primary;
    }
}

/* ---- File ring helpers ------------------------------------------------ */

static void ring_push(bs_log_lvl_t lvl, const char* tag, const char* msg) {
    int slot = s_ring_head;
    snprintf(s_ring[slot], LOG_LINE_MAX, "%s %s: %s\n",
             level_tag(lvl), tag ? tag : "", msg);
    s_ring_lvl[slot] = lvl;
    s_ring_head = (slot + 1) % LOG_BUF_LINES;
    if (s_ring_count < LOG_BUF_LINES) s_ring_count++;
}

static void file_append(const char* line) {
    bs_file_t f = bs_fs_open(LOG_FILE, "a");
    if (!f) return;
    bs_fs_write(f, line, (int)strlen(line));
    bs_fs_close(f);
}

/* Re-render the last (n) ring entries (excluding the final one) into the boot
 * area, starting from s_boot_start_y.  Used during auto-scroll. */
static void boot_redraw_history(int n, int line_h, int sw, int ts) {
    /* The ring already contains the new entry at head-1.  We render the
     * (n) entries before it — i.e. indices [count-1-n .. count-2]. */
    int start = s_ring_count - 1 - n;
    if (start < 0) start = 0;
    s_boot_y = s_boot_start_y;
    for (int i = start; i < s_ring_count - 1; i++) {
        int idx = (s_ring_head - s_ring_count + i + LOG_BUF_LINES) % LOG_BUF_LINES;
        char buf[LOG_LINE_MAX];
        int  rlen = (int)strlen(s_ring[idx]);
        if (rlen > 0 && s_ring[idx][rlen - 1] == '\n') rlen--;
        if (rlen > (int)sizeof(buf) - 1) rlen = (int)sizeof(buf) - 1;
        memcpy(buf, s_ring[idx], (size_t)rlen);
        buf[rlen] = '\0';
        bs_gfx_fill_rect(0, s_boot_y, sw, line_h, g_bs_theme.bg);
        bs_gfx_text(2, s_boot_y, buf, bs_log_level_color(s_ring_lvl[idx]), ts);
        s_boot_y += line_h;
    }
}

/* ---- Public API ------------------------------------------------------- */

void bs_log_init(struct konsole* ks) {
    s_ks = ks;
    s_boot_y = s_boot_start_y = bs_gfx_height() * BOOT_LOG_START_Y_FRAC / 100;
}

void bs_log_boot_reset(int start_y) {
    s_boot_y = s_boot_start_y = start_y;
}

void bs_log(bs_log_lvl_t lvl, const char* tag, const char* fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* ---- UART output ---- */
    bool is_boot = (lvl >= BS_LOG_BOOT_OK);
    if (s_ks) {
        if (is_boot) {
            kon_printf(s_ks, "%s%s\033[0m %s: %s\r\n",
                       level_ansi(lvl), level_tag(lvl), tag ? tag : "", msg);
        } else {
            kon_printf(s_ks, "%s[%s]\033[0m %s: %s\r\n",
                       level_ansi(lvl),
                       (lvl == BS_LOG_WARN) ? "WARN" :
                       (lvl == BS_LOG_ERR)  ? "ERR"  : "INFO",
                       tag ? tag : "", msg);
        }
    }

    /* ---- Ring buffer ---- */
    ring_push(lvl, tag, msg);
    if (s_fs_ready) file_append(s_ring[(s_ring_head - 1 + LOG_BUF_LINES) % LOG_BUF_LINES]);

    /* ---- Boot display (scale=1, auto-scroll) ---- */
    if (!is_boot) return;

    const int ts     = 1;   /* boot log always small — use app_log for large text */
    const int th     = bs_gfx_text_h(ts);
    const int line_h = th + 2;
    const int sw     = bs_gfx_width();
    const int sh     = bs_gfx_height();
    const int char_w = 6 * ts;

    /* Compose display line */
    char line[128];
    snprintf(line, sizeof line, "%s %s: %s", level_tag(lvl), tag ? tag : "", msg);
    int max_cols = (sw - 4) / char_w;
    if (max_cols < 1) max_cols = 1;
    int len = (int)strlen(line);
    int nchunks = (len + max_cols - 1) / max_cols;
    if (nchunks < 1) nchunks = 1;

    /* Auto-scroll: if the new entry would overflow, clear the area and
     * re-render the last fitting history entries to keep the view at bottom. */
    if (s_boot_y + nchunks * line_h > sh) {
        int area_h    = sh - s_boot_start_y;
        int lines_fit = area_h / line_h;
        int history   = lines_fit - nchunks;
        bs_gfx_fill_rect(0, s_boot_start_y, sw, area_h, g_bs_theme.bg);
        boot_redraw_history(history, line_h, sw, ts);
    }

    /* Draw the new line (may wrap) */
    bs_color_t col = bs_log_level_color(lvl);
    for (int start = 0; start < len; start += max_cols) {
        char chunk[129];
        int n = len - start;
        if (n > max_cols) n = max_cols;
        memcpy(chunk, line + start, (size_t)n);
        chunk[n] = '\0';
        bs_gfx_fill_rect(0, s_boot_y, sw, line_h, g_bs_theme.bg);
        bs_gfx_text(2, s_boot_y, chunk, col, ts);
        bs_gfx_present();
        s_boot_y += line_h;
    }
}

void bs_log_flush_sd(void) {
    if (!bs_fs_available()) return;
    if (bs_fs_file_size(LOG_FILE) > LOG_MAX_BYTES)
        bs_fs_remove(LOG_FILE);
    bs_file_t f = bs_fs_open(LOG_FILE, "a");
    if (!f) return;
    bs_fs_write(f, "--- boot ---\n", 13);
    for (int i = 0; i < s_ring_count; i++) {
        int idx = (s_ring_head - s_ring_count + i + LOG_BUF_LINES) % LOG_BUF_LINES;
        bs_fs_write(f, s_ring[idx], (int)strlen(s_ring[idx]));
    }
    bs_fs_close(f);
    s_fs_ready = true;
}

void bs_log_print_all(struct konsole* ks) {
    for (int i = 0; i < s_ring_count; i++) {
        int idx = (s_ring_head - s_ring_count + i + LOG_BUF_LINES) % LOG_BUF_LINES;
        const char* line = s_ring[idx];
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n') len--;
        kon_printf(ks, "%.*s\r\n", len, line);
    }
}

/* ---- Public ring access (for app_log viewer) -------------------------- */

int bs_log_count(void) {
    return s_ring_count;
}

const char* bs_log_entry(int i) {
    if (i < 0 || i >= s_ring_count) return NULL;
    int idx = (s_ring_head - s_ring_count + i + LOG_BUF_LINES) % LOG_BUF_LINES;
    return s_ring[idx];
}

bs_log_lvl_t bs_log_entry_lvl(int i) {
    if (i < 0 || i >= s_ring_count) return BS_LOG_INFO;
    int idx = (s_ring_head - s_ring_count + i + LOG_BUF_LINES) % LOG_BUF_LINES;
    return s_ring_lvl[idx];
}
