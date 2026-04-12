#pragma once
/*
 * wifi_captive_svc.h - Manual captive-portal service (no UI).
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
 * Start the captive portal AP.
 *   ssid     - AP name (max 32 chars); default "FreeWifi"
 *   ch       - WiFi channel 1-13
 *   password - WPA2 PSK (8-63 chars); NULL / "" → open AP
 * Returns true on success.
 */
bool captive_svc_start(const char* ssid, uint8_t ch, const char* password);
void captive_svc_stop(void);

/* Drive portal HTTP/DNS polling.  Call from hl_tick every main-loop iteration. */
void captive_svc_tick(uint32_t now_ms);

bool                      captive_svc_active(void);
int                       captive_svc_client_count(void);
int                       captive_svc_cred_count(void);
const wifi_portal_cred_t* captive_svc_get_cred(int idx);

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
