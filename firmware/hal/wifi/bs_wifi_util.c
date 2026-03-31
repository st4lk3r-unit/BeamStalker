/*
 * bs_wifi_util.c - WiFi utility functions, always compiled (no backend guard).
 *
 * These helpers are pure C with no hardware dependency and are shared by
 * all WiFi backends and by apps that need display-friendly strings.
 */
#include "bs/bs_wifi.h"
#include <stdio.h>

void bs_wifi_bssid_str(const uint8_t bssid[6], char* buf18) {
    snprintf(buf18, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

const char* bs_wifi_auth_str(bs_wifi_auth_t auth) {
    switch (auth) {
        case BS_WIFI_AUTH_OPEN:          return "OPEN";
        case BS_WIFI_AUTH_WEP:           return "WEP";
        case BS_WIFI_AUTH_WPA_PSK:       return "WPA";
        case BS_WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case BS_WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
        case BS_WIFI_AUTH_WPA3_PSK:      return "WPA3";
        default:                         return "?";
    }
}
