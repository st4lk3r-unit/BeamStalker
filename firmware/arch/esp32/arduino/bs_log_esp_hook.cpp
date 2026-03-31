/*
 * bs_log_esp_hook.cpp - redirect ESP-IDF/Arduino esp_log output into bs_log.
 *
 * Without this hook, ESP_LOGx / log_e / log_w messages (e.g. from sd_diskio,
 * vfs, WiFi) are printed directly to UART and are invisible in bs_log's ring
 * buffer and the konsole `log` command.
 *
 * esp_log_set_vprintf() installs a vprintf-compatible function that receives
 * the already-formatted log line (including the [E][file:line] prefix).
 * We strip the trailing \r\n and push the line into bs_log as BS_LOG_ERR or
 * BS_LOG_WARN based on the level letter, keeping everything else as INFO.
 */
#ifdef ARCH_ESP32

extern "C" {
#include "bs/bs_log.h"
}

#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int s_in_hook = 0;  /* re-entry guard */

static int esp_log_hook(const char* fmt, va_list args) {
    if (s_in_hook) {
        /* Fallback: write directly so we don't lose the message */
        return vprintf(fmt, args);
    }
    s_in_hook = 1;

    char buf[192];
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    if (n > 0) {
        buf[n] = '\0';
        /* Strip trailing whitespace / CRLF */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
            buf[--n] = '\0';

        if (n > 0) {
            /*
             * Arduino ESP32 format: "[  1369][E][sd_diskio.cpp:126] message"
             * IDF format:           "E (1369) tag: message"
             * Detect level from first non-space character or bracketed letter.
             */
            bs_log_lvl_t lvl = BS_LOG_INFO;
            /* Arduino format: second bracket contains the level letter */
            const char* lb = strchr(buf, ']');
            if (lb && lb[1] == '[') {
                char c = lb[2];
                if      (c == 'E') lvl = BS_LOG_ERR;
                else if (c == 'W') lvl = BS_LOG_WARN;
            } else if (buf[0] == 'E' && buf[1] == ' ') {
                lvl = BS_LOG_ERR;
            } else if (buf[0] == 'W' && buf[1] == ' ') {
                lvl = BS_LOG_WARN;
            }
            bs_log(lvl, "esp", "%s", buf);
        }
    }

    s_in_hook = 0;
    return n < 0 ? 0 : n;
}

extern "C" void bs_log_esp_hook_init(void) {
    esp_log_set_vprintf(esp_log_hook);
}

#endif /* ARCH_ESP32 */
