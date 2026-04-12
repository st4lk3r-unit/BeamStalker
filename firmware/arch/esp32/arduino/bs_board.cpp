#ifdef ARCH_ESP32

extern "C" {
#include "bs/bs_board.h"
#include "board.h"
}

#include <Arduino.h>

#ifdef BS_USE_SIC
extern "C" {
#  include <sic/sic.h>
#  include <sic/bus/i2c_bus.h>
}
#endif

extern "C" int bs_board_platform_init(const bs_arch_t* arch) {
    (void)arch;
#if defined(BS_DISPLAY_POWER_PIN)
    pinMode(BS_DISPLAY_POWER_PIN, OUTPUT);
#  if defined(BS_DISPLAY_POWER_ACTIVE_LOW) && BS_DISPLAY_POWER_ACTIVE_LOW
    digitalWrite(BS_DISPLAY_POWER_PIN, LOW);
#  else
    digitalWrite(BS_DISPLAY_POWER_PIN, HIGH);
#  endif
#endif
#ifdef BS_USE_SIC
    sic_i2c_begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
    sic_begin_legacy(&BS_SIC_BOARD, NULL);
#endif
    return 0;
}

static void deselect_cs_if_valid(int pin) {
    if (pin < 0) return;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
}

extern "C" void bs_board_platform_prepare_fs_mount(void) {
#if defined(SGFX_PIN_CS) && SGFX_PIN_CS >= 0
    deselect_cs_if_valid(SGFX_PIN_CS);
#endif
#if defined(BS_LORA_CS_PIN) && BS_LORA_CS_PIN >= 0
    deselect_cs_if_valid(BS_LORA_CS_PIN);
#endif
#if defined(BS_NFC_CS_PIN) && BS_NFC_CS_PIN >= 0
    deselect_cs_if_valid(BS_NFC_CS_PIN);
#endif
    delay(50);
}

extern "C" int bs_board_platform_sd_detect(void) {
#ifdef BS_USE_SIC
    uint8_t reg = 0x01, val = 0xFF;
    if (sic_i2c_writeread(0, 0x20, &reg, 1, &val, 1) < 0) return -1;
    return (val & (1 << 2)) ? 0 : 1;
#else
    return -1;
#endif
}

extern "C" void bs_board_platform_diag_read(bs_board_diag_t* out) {
    if (!out) return;
    out->available = 0;
    out->reachable = 0;
    out->input_1 = out->output_1 = out->config_1 = 0xFF;
#if defined(BS_USE_SIC)
    out->available = 1;
    uint8_t reg = 0x01;
    if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->input_1, 1) < 0) return;
    reg = 0x03;
    if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->output_1, 1) < 0) return;
    reg = 0x07;
    if (sic_i2c_writeread(0, 0x20, &reg, 1, &out->config_1, 1) < 0) return;
    out->reachable = 1;
#endif
}

#endif /* ARCH_ESP32 */
