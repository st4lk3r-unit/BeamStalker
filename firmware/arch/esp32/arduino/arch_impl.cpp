/*
 * arch/esp32/arduino/arch_impl.cpp - Arduino (ESP32) arch vtable.
 */
#include <Arduino.h>
#include "bs/bs_arch.h"
#include "board.h"

extern "C" {

static uint32_t s_start_ms = 0;

static int arduino_init(void) {
    s_start_ms = millis();
#ifdef BS_UART_CONSOLE_IDX
    if (BS_UART_CONSOLE_IDX == 0) {
        Serial.begin(BS_UART_BAUD_VAL);
        unsigned long t0 = millis();
        while (!Serial && (millis() - t0) < 1500) delay(10);
    }
#endif
    return 0;
}

static void arduino_delay_ms(uint32_t ms) { delay(ms); }
static uint32_t arduino_millis(void) { return millis() - s_start_ms; }

static int arduino_uart_init(int idx, uint32_t baud) {
    (void)baud;
    if (idx == 0) { if (!Serial) Serial.begin(baud); return 0; }
    return -1;
}

static int arduino_uart_write(int idx, const void* buf, size_t len) {
    if (idx != 0 || !buf || !len) return 0;
    return (int)Serial.write((const uint8_t*)buf, len);
}

static int arduino_uart_read(int idx, void* buf, size_t len) {
    if (idx != 0 || !buf || !len) return 0;
    size_t n = 0;
    while (Serial.available() && n < len)
        ((uint8_t*)buf)[n++] = (uint8_t)Serial.read();
    return (int)n;
}

static const bs_arch_t API = {
    .init       = arduino_init,
    .delay_ms   = arduino_delay_ms,
    .millis     = arduino_millis,
    .uart_init  = arduino_uart_init,
    .uart_write = arduino_uart_write,
    .uart_read  = arduino_uart_read,
};

const bs_arch_t* arch_bs(void) { return &API; }

/* konsole weak-override: hard-reset the chip on 'reboot' command */
void konsole_on_reboot(void) {
    esp_restart();
}

} /* extern "C" */
