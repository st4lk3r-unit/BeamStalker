#pragma once
/*
 * bs_arch.h - BeamStalker platform abstraction vtable.
 *
 * Same contract as neutrino's arch_api but scoped to BeamStalker.
 * Each arch port provides arch_bs() returning a const pointer to its vtable.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      (*init)(void);
    void     (*delay_ms)(uint32_t ms);
    uint32_t (*millis)(void);

    /* UART - index 0 = primary console/debug serial */
    int  (*uart_init)(int idx, uint32_t baud);
    int  (*uart_write)(int idx, const void* buf, size_t len);
    int  (*uart_read)(int idx, void* buf, size_t len);  /* non-blocking */
} bs_arch_t;

/* Implemented by each arch port (native/posix, esp32/arduino, …) */
const bs_arch_t* arch_bs(void);

#ifdef __cplusplus
}
#endif
