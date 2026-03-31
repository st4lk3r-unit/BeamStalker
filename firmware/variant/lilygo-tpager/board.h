#pragma once
/* variant/lilygo-tpager/board.h — LilyGO T-Pager (ESP32-S3, ST7796 480×222) */

#define BS_BOARD_NAME        "lilygo-tpager"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200

/* SIC board descriptor */
#ifdef BS_USE_SIC
#  include <sic/sic_board.h>
#  define BS_SIC_BOARD SIC_BOARD_TPAGER
#endif

/* Rotary encoder GPIO pins (CLK/A=40, DT/B=41) — enables ISR-driven reading */
#define BS_ENC_PIN_A  40
#define BS_ENC_PIN_B  41
