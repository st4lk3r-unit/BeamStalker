#pragma once
/*
 * wifi_eviltwin_svc.h - Evil Twin AP + deauth service (no UI).
 *
 * Starts a cloned AP with an optional WPA2 PSK, then sends periodic
 * broadcast deauth + disassoc frames spoofed from the real AP's BSSID
 * to push clients onto our clone.
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
 * Start the evil-twin AP.
 *   ssid        - SSID to clone (max 32 chars); required
 *   ch          - channel (1-13); should match the real AP for seamless roam
 *   password    - WPA2 PSK (8-63 chars); NULL → open clone
 *   real_bssid  - real AP's BSSID to spoof in deauth frames;
 *                 NULL → clone AP only, no active deauth injection
 * Returns true on success.
 */
bool eviltwin_svc_start(const char* ssid, uint8_t ch,
                        const char* password,
                        const uint8_t real_bssid[6]);
void eviltwin_svc_stop(void);

/* Poll portal + send deauth burst.  Call from hl_tick every iteration. */
void eviltwin_svc_tick(uint32_t now_ms);

bool                      eviltwin_svc_active(void);
int                       eviltwin_svc_client_count(void);
int                       eviltwin_svc_cred_count(void);
const wifi_portal_cred_t* eviltwin_svc_get_cred(int idx);
uint32_t                  eviltwin_svc_deauth_total(void);

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
