#pragma once
/*
 * wifi_eapol.h - EAPOL handshake capture UI.
 *
 * Thin wrapper over wifi_eapol_svc.  Handles menu and running display.
 * Attack logic lives entirely in wifi_eapol_svc.c.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "bs/bs_arch.h"

void wifi_eapol_run(const bs_arch_t* arch);

#endif /* BS_HAS_WIFI */
