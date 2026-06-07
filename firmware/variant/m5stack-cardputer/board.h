#pragma once
/* M5Stack Cardputer v1.0 / v1.1 */
#define BS_BOARD_NAME        "m5stack-cardputer"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200
#define BS_BOARD_ARCH_DESC   "ESP32-S3 / M5Stack Cardputer"
#define BS_BOARD_KEYBOARD_DESC "SIC 74HC138 keyboard matrix"
#define BS_BOARD_CAP_FLAGS   (BS_BOARD_CAP_FAST_UI | BS_BOARD_CAP_SIC | BS_BOARD_CAP_SHARED_SD_SPI)
#define BS_BOARD_UI_IDLE_DELAY_MS 1

/* Backlight control.
 * GPIO38 is a display/backlight power gate on Cardputer-class boards.
 * Treat it as on/off only: PWM below 100% can black the LCD and, on
 * Stamp-S3A/ADV, shares power with the RGB LED rail.
 */
#define BS_BACKLIGHT_GPIO_ONLY 1
#define BS_SGFX_BL_ACTIVE_LOW  0

#ifdef BS_USE_SIC
#  include <sic/sic_board.h>
#  define BS_SIC_BOARD SIC_BOARD_CARDPUTER
#endif
