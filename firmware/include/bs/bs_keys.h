#pragma once
/*
 * bs_keys.h - BeamStalker firmware-level keystroke abstraction.
 *
 * Normalises input across platforms:
 *   BS_KEYS_NATIVE : raw stdin (native Linux)
 *   BS_KEYS_SIC    : TCA8418 keyboard + rotary encoder (hardware via SIC)
 *   BS_KEYS_GPIO   : minimal GPIO-backed button input (single-button boards)
 *
 * bs_keys_poll() is non-blocking. Call it every loop iteration.
 */
#include <stdint.h>
#include <stdbool.h>
#include "bs_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BS_KEY_NONE  = 0,
    BS_KEY_UP,
    BS_KEY_DOWN,
    BS_KEY_LEFT,
    BS_KEY_RIGHT,
    BS_KEY_ENTER,
    BS_KEY_BACK,
    BS_KEY_ESC,
    BS_KEY_FUNC,    /* orange/alt modifier key on T-Pager */
    BS_KEY_CHAR,    /* printable character - inspect .ch  */
} bs_key_id_t;

typedef struct {
    bs_key_id_t id;
    char        ch;   /* valid when id == BS_KEY_CHAR */
} bs_key_t;

void bs_keys_init(const bs_arch_t* arch);

/*
 * Poll for one keystroke.  Returns true and writes *out if a key is
 * available; returns false immediately when the input queue is empty.
 */
bool bs_keys_poll(bs_key_t* out);

#ifdef __cplusplus
}
#endif
