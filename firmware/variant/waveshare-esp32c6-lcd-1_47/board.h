#pragma once
/* variant/waveshare-esp32c6-lcd-1_47/board.h — Waveshare ESP32-C6-LCD-1.47 */

#define BS_BOARD_NAME          "waveshare-esp32c6-lcd-1.47"
#define BS_UART_CONSOLE_IDX    0
#define BS_UART_BAUD_VAL       115200
#define BS_BOARD_ARCH_DESC     "ESP32-C6 / Waveshare ESP32-C6-LCD-1.47"
#define BS_BOARD_KEYBOARD_DESC "single BOOT key + serial konsole"
#define BS_BOARD_CAP_FLAGS     (BS_BOARD_CAP_SINGLE_KEY | BS_BOARD_CAP_SHARED_SD_SPI)
#define BS_BOARD_UI_IDLE_DELAY_MS 2

/* The non-touch LCD-1.47 board has BOOT + RESET only.  BOOT is usable as a
 * minimal navigation key, matching the existing single-key BeamStalker flow:
 *   short press  -> NEXT / RIGHT
 *   double press -> BACK
 *   long press   -> ENTER
 */
#define BS_GPIOKEY_PIN         9
#define BS_GPIOKEY_ACTIVE_LOW  1
#define BS_GPIOKEY_LONG_MS     650
#define BS_GPIOKEY_DOUBLE_MS   300

/* ST7789 backlight is on GPIO22.  Keep PWM enabled: unlike the Cardputer
 * GPIO38 rail, this is a normal LCD backlight control pin on this board.
 */
#define BS_BACKLIGHT_PWM       1
#define BS_SGFX_BL_ACTIVE_LOW  0
/* ESP32-C6 exposes fewer LEDC channels than ESP32-S3.  Use channel 0 and
 * keep this as a variant trait, not a board-name branch in the HAL.
 */
#define BS_BACKLIGHT_PWM_CHANNEL 0
#define BS_BACKLIGHT_PWM_TIMER   0

/* Useful onboard helpers for future status LED support. */
#define BS_RGB_LED_PIN         8
#define BS_LCD_BL_PIN          22
#define BS_LCD_RST_PIN         21
#define BS_LCD_DC_PIN          15
#define BS_LCD_CS_PIN          14
#define BS_LCD_SCK_PIN         7
#define BS_LCD_MOSI_PIN        6
#define BS_TF_CS_PIN           4
#define BS_TF_MISO_PIN         5
