/*
 * bs_wifi_nrf.c - WiFi backend stub for nRF7002 companion IC.
 *
 * The nRF7002 is a 2.4/5 GHz Wi-Fi 6 companion used alongside nRF5340.
 * It runs on Zephyr RTOS with the nRF Connect SDK.
 *
 * Capabilities (once implemented):
 *   BS_WIFI_CAP_CONNECT  — net_mgmt NET_REQUEST_WIFI_CONNECT
 *   BS_WIFI_CAP_SCAN     — net_mgmt NET_REQUEST_WIFI_SCAN
 *
 * NOT supported by nRF7002 hardware:
 *   BS_WIFI_CAP_SNIFF    — no promiscuous mode in the nRF7002 SDK
 *   BS_WIFI_CAP_MONITOR  — no monitor mode
 *   BS_WIFI_CAP_INJECT   — no raw frame transmit
 *
 * TODO: integrate nRF Connect SDK net_mgmt WiFi management API.
 *   Required headers: <zephyr/net/wifi_mgmt.h>, <zephyr/net/net_if.h>
 *   Required Kconfig: CONFIG_WIFI=y, CONFIG_WPA_SUPLICANT=y
 *
 * References:
 *   https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/drivers/wifi/nrf700x/index.html
 */
#ifdef BS_WIFI_NRF7002

#include "bs/bs_wifi.h"
#include <string.h>

/* TODO: replace with actual Zephyr net_mgmt includes
 * #include <zephyr/net/wifi_mgmt.h>
 * #include <zephyr/net/net_if.h>
 */

static bs_wifi_state_t s_state = BS_WIFI_STATE_OFF;

int bs_wifi_init(const bs_arch_t* arch) {
    (void)arch;
    /* TODO: net_if_get_default(), register net_mgmt events */
    s_state = BS_WIFI_STATE_IDLE;
    return 0;
}

void bs_wifi_deinit(void) {
    /* TODO: unregister net_mgmt events */
    s_state = BS_WIFI_STATE_OFF;
}

uint32_t bs_wifi_caps(void) {
    return BS_WIFI_CAP_CONNECT | BS_WIFI_CAP_SCAN;
}

bs_wifi_state_t bs_wifi_state(void) {
    /* TODO: poll net_mgmt for connect/disconnect events */
    return s_state;
}

int bs_wifi_scan_start(void) {
    /* TODO: net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0) */
    (void)s_state;
    return -1;  /* not yet implemented */
}

bool bs_wifi_scan_done(void) {
    /* TODO: check NET_EVENT_WIFI_SCAN_DONE */
    return false;
}

int bs_wifi_scan_results(bs_wifi_ap_t* out, int max_count) {
    /* TODO: read results from NET_EVENT_WIFI_SCAN_RESULT callbacks */
    (void)out; (void)max_count;
    return -1;
}

int bs_wifi_connect(const char* ssid, const char* password) {
    /* TODO: struct wifi_connect_req_params params = { .ssid = ssid, ... };
     *       net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof params); */
    (void)ssid; (void)password;
    return -1;
}

void bs_wifi_disconnect(void) {
    /* TODO: net_mgmt(NET_REQUEST_WIFI_DISCONNECT, ...) */
}

int bs_wifi_get_ip(char* buf, size_t len) {
    /* TODO: net_if_ipv4_addr_lookup() */
    (void)buf; (void)len;
    return -1;
}

/* Unsupported capabilities — return -1 cleanly */

int  bs_wifi_monitor_start(uint8_t ch, bs_wifi_frame_cb_t cb, void* ctx)
     { (void)ch; (void)cb; (void)ctx; return -1; }
void bs_wifi_monitor_stop(void)    {}

int  bs_wifi_sniff_start(uint8_t ch, bs_wifi_frame_cb_t cb, void* ctx)
     { (void)ch; (void)cb; (void)ctx; return -1; }
void bs_wifi_sniff_stop(void)      {}

int  bs_wifi_set_tx_power(int dbm)                 { (void)dbm; return -1; }
int  bs_wifi_set_channel(uint8_t ch)               { (void)ch; return -1; }
int  bs_wifi_send_raw(bs_wifi_if_t iface, const uint8_t* f, uint16_t l)
     { (void)iface; (void)f; (void)l; return -1; }

#endif /* BS_WIFI_NRF7002 */
