#pragma once
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#include "bs/bs_arch.h"

/* Run the Deauther sub-application. Returns when the user exits. */
void wifi_deauth_run(const bs_arch_t* arch);

#endif /* BS_HAS_WIFI */
