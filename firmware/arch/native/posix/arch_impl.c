/*
 * arch/native/posix/arch_impl.c - POSIX (Linux/macOS) arch vtable.
 *
 * UART idx 0 → stdin/stdout.
 * stdin put in raw mode at init so key input works non-line-buffered.
 * Note: when BS_GFX_NATIVE is active, the konsole layer redirects its
 * output to stderr independently.  stdin raw mode is still set here so
 * bs_keys_native can read single keystrokes without pressing Enter.
 */
#include <unistd.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include "bs/bs_arch.h"

static uint32_t s_start_ms;

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

static struct termios s_orig_tio;
static int            s_tty_raw = 0;

static void set_stdin_raw(void) {
    if (s_tty_raw) return;
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &s_orig_tio) == 0) {
        tio = s_orig_tio;
        cfmakeraw(&tio);
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        s_tty_raw = 1;
    }
}

static void restore_stdin(void) {
    if (s_tty_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &s_orig_tio);
        s_tty_raw = 0;
    }
}

static int posix_init(void) {
    s_start_ms = now_ms();
    set_stdin_raw();
    atexit(restore_stdin);
    return 0;
}

static void posix_delay_ms(uint32_t ms) {
    struct timespec ts = {
        .tv_sec  = (time_t)(ms / 1000),
        .tv_nsec = (long)((ms % 1000) * 1000000L)
    };
    nanosleep(&ts, NULL);
}

static uint32_t posix_millis(void) {
    return now_ms() - s_start_ms;
}

static int posix_uart_init(int idx, uint32_t baud) {
    (void)idx; (void)baud;
    return 0;
}

static int posix_uart_write(int idx, const void* buf, size_t len) {
    (void)idx;
    if (!buf || !len) return 0;
    ssize_t w = write(STDOUT_FILENO, buf, len);
    return (int)(w < 0 ? 0 : w);
}

static int posix_uart_read(int idx, void* buf, size_t len) {
    (void)idx;
    if (!buf || !len) return 0;
    fd_set rfds;
    struct timeval tv = {0, 0};
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) return 0;
    ssize_t n = read(STDIN_FILENO, buf, len);
    return (int)(n < 0 ? 0 : n);
}

static const bs_arch_t API = {
    .init       = posix_init,
    .delay_ms   = posix_delay_ms,
    .millis     = posix_millis,
    .uart_init  = posix_uart_init,
    .uart_write = posix_uart_write,
    .uart_read  = posix_uart_read,
};

const bs_arch_t* arch_bs(void) { return &API; }

/* konsole weak-override: exit the process cleanly on 'reboot' command */
void konsole_on_reboot(void) {
    restore_stdin();
    exit(0);
}
