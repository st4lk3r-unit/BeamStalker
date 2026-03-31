#pragma once
#include "bs/bs_arch.h"
#ifdef __cplusplus
extern "C" {
#endif

/*
 * wifi_eviltwin_run - Evil Twin sub-application entry point.
 *
 * Scans for APs, lets the user pick a target, optionally enters a WPA2 PSK,
 * then clones the AP (same SSID + channel, open or WPA2) and runs a captive
 * portal while continuously deauthing clients off the real AP.
 */
void wifi_eviltwin_run(const bs_arch_t* arch);

#ifdef __cplusplus
}
#endif
