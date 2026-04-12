/*
 * wifi_captive_svc.c - Manual captive-portal service layer (no UI).
 *
 * Thin wrapper around wifi_portal_*: start AP, serve DNS+HTTP, track creds.
 * All logic lives in wifi_portal; this svc layer exists purely so beamstalker.c
 * can drive it through the uniform hl_mode / hl_tick pattern.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI
#if defined(BS_WIFI_ESP32) && defined(ARDUINO_ARCH_ESP32)

#include "wifi_captive_svc.h"

static bool s_active = false;

bool captive_svc_start(const char* ssid, uint8_t ch, const char* password) {
    if (s_active) captive_svc_stop();
    s_active = wifi_portal_start(ssid, ch, password);
    return s_active;
}

void captive_svc_stop(void) {
    wifi_portal_stop();
    s_active = false;
}

void captive_svc_tick(uint32_t now_ms) {
    if (!s_active) return;
    static uint32_t s_last_poll = 0;
    if (s_last_poll == 0 || (now_ms - s_last_poll) >= 10) {
        s_last_poll = now_ms;
        wifi_portal_poll();
    }
}

bool captive_svc_active(void) {
    return s_active && wifi_portal_active();
}

int captive_svc_client_count(void) {
    return wifi_portal_client_count();
}

int captive_svc_cred_count(void) {
    return wifi_portal_cred_count();
}

const wifi_portal_cred_t* captive_svc_get_cred(int idx) {
    return wifi_portal_get_cred(idx);
}

#endif /* BS_WIFI_ESP32 && ARDUINO_ARCH_ESP32 */
#endif /* BS_HAS_WIFI */
