/*
 * bs_keys_none.c - No-op keys backend for headless / konsole-only builds.
 *
 * Activated when neither BS_KEYS_NATIVE nor BS_KEYS_SIC is defined.
 * In headless mode input arrives through the konsole UART path, not
 * through bs_keys, so all key poll calls are silent no-ops.
 *
 * Build flag:  define nothing — absence of BS_KEYS_NATIVE, BS_KEYS_SIC, and BS_KEYS_GPIO
 *              activates this backend automatically.
 */
#if !defined(BS_KEYS_NATIVE) && !defined(BS_KEYS_SIC) && !defined(BS_KEYS_GPIO)

#include "bs/bs_keys.h"

void bs_keys_init(const bs_arch_t* arch) { (void)arch; }
bool bs_keys_poll(bs_key_t* out)         { (void)out; return false; }

#endif /* !BS_KEYS_NATIVE && !BS_KEYS_SIC && !BS_KEYS_GPIO */
