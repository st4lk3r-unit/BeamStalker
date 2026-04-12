#pragma once
/*
 * wifi_honeypot_svc.h - Rogue AP + CSA/deauth lure injection service (no UI).
 *
 * Clones a target AP on a non-overlapping channel and periodically hops back
 * to the real AP's channel to inject CSA beacons + deauth frames, pushing
 * lingering clients toward the honeypot.
 *
 * Only compiled when BS_WIFI_ESP32 + ARDUINO_ARCH_ESP32.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include <stdbool.h>
#include <stdint.h>
#include "wifi_portal.h"

typedef enum {
    HONEYPOT_SVC_BROADCAST = 0,  /* deauth to FF:FF:FF:FF:FF:FF (fast)       */
    HONEYPOT_SVC_TARGETED  = 1,  /* directed deauth to clients observed on
                                    the target channel during lure hops        */
} honeypot_svc_mode_t;

/*
 * Start the honeypot.
 *   target_ssid  - SSID of the AP to clone
 *   target_bssid - BSSID of the real AP (used in spoofed CSA + deauth frames)
 *   target_ch    - channel of the real AP (1-13)
 *   hp_ch        - channel for the fake AP; 0 = auto-pick from {1,6,11}
 *   mode         - BROADCAST or TARGETED deauth style
 * Returns true on success.
 */
bool honeypot_svc_start(const char*         target_ssid,
                        const uint8_t       target_bssid[6],
                        uint8_t             target_ch,
                        uint8_t             hp_ch,
                        honeypot_svc_mode_t mode);
void honeypot_svc_stop(void);

/* Poll portal + fire lure timer.  Call from hl_tick every iteration. */
void honeypot_svc_tick(uint32_t now_ms);

bool                      honeypot_svc_active(void);
int                       honeypot_svc_client_count(void);
int                       honeypot_svc_cred_count(void);
const wifi_portal_cred_t* honeypot_svc_get_cred(int idx);
uint32_t                  honeypot_svc_lure_count(void);  /* injections fired */

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
