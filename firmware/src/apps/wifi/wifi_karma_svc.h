#pragma once
/*
 * wifi_karma_svc.h - KARMA attack service (no UI).
 *
 * Runs an open captive-portal AP and responds to directed WiFi probe requests
 * with unicast probe responses, making nearby devices see the AP immediately
 * rather than waiting for a broadcast beacon.
 *
 * Two modes:
 *   specific - AP responds only to probes for the supplied SSID
 *   auto     - AP responds to probes for any SSID seen since start
 *
 * Only compiled when BS_WIFI_ESP32 + ARDUINO_ARCH_ESP32.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include <stdbool.h>
#include <stdint.h>
#include "wifi_portal.h"

/*
 * Start the KARMA AP.
 *   ssid      - AP name; if auto_mode=false this is also the only SSID matched
 *               in probes; if auto_mode=true it seeds the initial SSID list
 *   ch        - AP channel (1-13)
 *   auto_mode - true: respond to probes for any SSID seen; add new SSIDs as
 *               they appear in probe requests
 * Returns true on success.
 */
bool karma_svc_start(const char* ssid, uint8_t ch, bool auto_mode);
void karma_svc_stop(void);

/* Poll portal + dispatch pending probe responses.  Call from hl_tick. */
void karma_svc_tick(uint32_t now_ms);

bool                      karma_svc_active(void);
int                       karma_svc_client_count(void);
int                       karma_svc_cred_count(void);
const wifi_portal_cred_t* karma_svc_get_cred(int idx);
int                       karma_svc_ssid_count(void);  /* unique probe SSIDs collected */
uint32_t                  karma_svc_probe_count(void); /* probe responses sent         */

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
