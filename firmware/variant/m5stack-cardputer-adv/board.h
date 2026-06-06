#pragma once
/* M5Stack Cardputer-ADV */
#define BS_BOARD_NAME        "m5stack-cardputer-adv"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200
#define BS_BOARD_ARCH_DESC   "ESP32-S3 / M5Stack Cardputer-ADV"
#define BS_BOARD_KEYBOARD_DESC "SIC TCA8418 keyboard"
#define BS_BOARD_CAP_FLAGS   (BS_BOARD_CAP_FAST_UI | BS_BOARD_CAP_SIC | BS_BOARD_CAP_SHARED_SD_SPI)
#define BS_BOARD_UI_IDLE_DELAY_MS 1

#ifdef BS_USE_SIC
#  include <sic/sic_board.h>
#  define BS_SIC_BOARD SIC_BOARD_CARDPUTER_ADV
#endif
