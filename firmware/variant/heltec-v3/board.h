#pragma once
/* variant/heltec-v3/board.h — Heltec WiFi LoRa 32 V3 (ESP32-S3, SSD1306 128×64) */

#define BS_BOARD_NAME        "heltec-v3"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200

/* Single onboard PRG button.
 * Mapping implemented in bs_keys_gpio.c:
 *   short press  -> NEXT / RIGHT
 *   double press -> BACK
 *   long press   -> ENTER
 */
#define BS_GPIOKEY_PIN         0
#define BS_GPIOKEY_ACTIVE_LOW  1
#define BS_GPIOKEY_LONG_MS     650
#define BS_GPIOKEY_DOUBLE_MS   300

/* OLED power is gated through Vext on Heltec V3.
 * LOW  = ON
 * HIGH = OFF
 */
#define BS_DISPLAY_POWER_PIN         36
#define BS_DISPLAY_POWER_ACTIVE_LOW  1

/* Useful onboard helpers for future board-specific bring-up. */
#define BS_LED_PIN               35
#define BS_OLED_RST_PIN          21
#define BS_OLED_SDA_PIN          17
#define BS_OLED_SCL_PIN          18
#define BS_LORA_CS_PIN           8
#define BS_LORA_SCK_PIN          9
#define BS_LORA_MOSI_PIN         10
#define BS_LORA_MISO_PIN         11
#define BS_LORA_RST_PIN          12
#define BS_LORA_BUSY_PIN         13
#define BS_LORA_DIO1_PIN         14
