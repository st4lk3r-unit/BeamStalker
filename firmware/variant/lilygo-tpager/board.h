#pragma once
/* variant/lilygo-tpager/board.h — LilyGO T-Pager (ESP32-S3, ST7796 480×222) */

#define BS_BOARD_NAME        "lilygo-tpager"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200
#define BS_BOARD_ARCH_DESC   "ESP32-S3 / LilyGO T-Pager"
#define BS_BOARD_KEYBOARD_DESC "SIC TCA8418 keyboard + rotary encoder"
#define BS_BOARD_CAP_FLAGS   (BS_BOARD_CAP_FAST_UI | BS_BOARD_CAP_SIC | BS_BOARD_CAP_ROTARY | BS_BOARD_CAP_XL9555_DIAG | BS_BOARD_CAP_SHARED_SD_SPI)
#define BS_BOARD_UI_IDLE_DELAY_MS 1

/* SIC board descriptor fallback. Normal init uses sic_board_default(), selected
 * by -DSIC_TARGET_TPAGER in platformio.ini. */
#ifdef BS_USE_SIC
#  include <sic/sic_board.h>
#  define BS_SIC_BOARD SIC_BOARD_TPAGER
#endif

/* Rotary encoder GPIO pins (CLK/A=40, DT/B=41) — enables ISR-driven reading */
#define BS_ENC_PIN_A  40
#define BS_ENC_PIN_B  41
