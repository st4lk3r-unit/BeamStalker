#pragma once
/* M5Stack Cardputer v1.0 */
#define BS_BOARD_NAME        "m5stack-cardputer"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200
#ifdef BS_USE_SIC
#  include <sic/sic_board.h>
#  define BS_SIC_BOARD SIC_BOARD_CARDPUTER
#endif
