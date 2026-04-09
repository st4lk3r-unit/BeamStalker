#pragma once
/* variant/lilygo-t-dongle-s3/board.h — LilyGO T-Dongle-S3 (ESP32-S3, ST7735 160×80) */

#define BS_BOARD_NAME        "lilygo-t-dongle-s3"
#define BS_UART_CONSOLE_IDX  0
#define BS_UART_BAUD_VAL     115200

/* Single onboard BOOT key used as a minimal UI navigation source.
 * Mapping implemented in bs_keys_gpio.c:
 *   short press  -> NEXT / RIGHT
 *   double press -> BACK
 *   long press   -> ENTER
 */
#define BS_GPIOKEY_PIN         0
#define BS_GPIOKEY_ACTIVE_LOW  1
#define BS_GPIOKEY_LONG_MS     650
#define BS_GPIOKEY_DOUBLE_MS   300

/* Panel/backlight traits (official LilyGO factory example uses GPIO38, active-low) */
#define BS_SGFX_BL_ACTIVE_LOW  0

/* T-Dongle-S3 integrated TF/uSD slot (SD_MMC, not SPI SD).
 * Not wired to BeamStalker FS yet; these are the official board pins for the
 * future SD_MMC backend.
 */
#define BS_SDMMC_D0_PIN        14
#define BS_SDMMC_D1_PIN        17
#define BS_SDMMC_D2_PIN        21
#define BS_SDMMC_D3_PIN        18
#define BS_SDMMC_CLK_PIN       12
#define BS_SDMMC_CMD_PIN       16
