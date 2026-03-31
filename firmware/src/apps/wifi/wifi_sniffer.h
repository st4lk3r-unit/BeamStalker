#pragma once
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#include "bs/bs_arch.h"

/* Run the Sniffer sub-application. Returns when the user exits. */
void wifi_sniffer_run(const bs_arch_t* arch);

#endif /* BS_HAS_WIFI */
