/*
 * bs_wifi_stm.c - WiFi backend stub for STM32 + WiFi module.
 *
 * Targets WiFi-capable STM32 boards such as:
 *   - B-U585I-IOT02A (EMW3080B module, MXCHIP)
 *   - B-L475E-IOT01A (ISM43362 module, Inventek)
 *   - Custom boards with ATWINC1500 or similar SPI/UART WiFi modules
 *
 * Capabilities (once implemented, hardware-dependent):
 *   BS_WIFI_CAP_CONNECT  — AT command or SDK WiFi connect
 *   BS_WIFI_CAP_SCAN     — AT command or SDK AP scan
 *
 * NOT supported by typical STM32 companion modules:
 *   BS_WIFI_CAP_SNIFF    — companion modules have no promiscuous mode
 *   BS_WIFI_CAP_MONITOR  — no monitor mode
 *   BS_WIFI_CAP_INJECT   — no raw frame transmit
 *
 * TODO: integrate the STM32 WiFi SDK (MXCHIP / ISM43362 / ATWINC HAL).
 *   For ST-provided modules, the MX_WIFI library is the reference.
 *
 * References:
 *   https://github.com/STMicroelectronics/STM32CubeU5 (MX_WIFI component)
 *   https://github.com/STMicroelectronics/x-cube-mxchip
 */
#ifdef BS_WIFI_STM

#include "bs/bs_wifi.h"
#include <string.h>

/* TODO: replace with actual STM32 WiFi SDK includes
 * #include "mx_wifi.h"   (MXCHIP)
 * or
 * #include "es_wifi.h"   (ISM43362 / Inventek)
 */

static bs_wifi_state_t s_state = BS_WIFI_STATE_OFF;

int bs_wifi_init(const bs_arch_t* arch) {
    (void)arch;
    /* TODO: MX_WIFI_Init() / ES_WIFI_Init() */
    s_state = BS_WIFI_STATE_IDLE;
    return 0;
}

void bs_wifi_deinit(void) {
    /* TODO: module power-down */
    s_state = BS_WIFI_STATE_OFF;
}

uint32_t bs_wifi_caps(void) {
    return BS_WIFI_CAP_CONNECT | BS_WIFI_CAP_SCAN;
}

bs_wifi_state_t bs_wifi_state(void) {
    /* TODO: poll module connection status */
    return s_state;
}

int bs_wifi_scan_start(void) {
    /* TODO: MX_WIFI_Scan() / ES_WIFI_ListAccessPoints() */
    return -1;
}

bool bs_wifi_scan_done(void) {
    /* TODO: check scan completion flag */
    return false;
}

int bs_wifi_scan_results(bs_wifi_ap_t* out, int max_count) {
    /* TODO: read scan results from module */
    (void)out; (void)max_count;
    return -1;
}

int bs_wifi_connect(const char* ssid, const char* password) {
    /* TODO: MX_WIFI_Connect() / ES_WIFI_Connect() */
    (void)ssid; (void)password;
    return -1;
}

void bs_wifi_disconnect(void) {
    /* TODO: MX_WIFI_Disconnect() */
}

int bs_wifi_get_ip(char* buf, size_t len) {
    /* TODO: MX_WIFI_GetIPAddress() */
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

#endif /* BS_WIFI_STM */
