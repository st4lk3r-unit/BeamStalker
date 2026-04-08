#pragma once
/*
 * wifi_beacon_svc.h - Beacon spam service layer.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"
#include "wifi_common.h"

typedef enum {
    BEACON_SVC_IDLE = 0,
    BEACON_SVC_RUNNING,
} beacon_svc_state_t;

typedef enum {
    BEACON_MODE_RANDOM = 0,
    BEACON_MODE_FILE,
    BEACON_MODE_CUSTOM,
} beacon_svc_mode_t;

void beacon_svc_init(const bs_arch_t* arch);
void beacon_svc_start(beacon_svc_mode_t mode, wifi_charset_t charset,
                      const char* prefix, int repeat,
                      const char (*file_ssids)[33], int file_ssid_count);
void beacon_svc_stop(void);
void beacon_svc_tick(uint32_t now_ms);

beacon_svc_state_t beacon_svc_state(void);
uint32_t           beacon_svc_frames(void);
uint32_t           beacon_svc_pps(void);
const char*        beacon_svc_cur_ssid(void);
uint8_t            beacon_svc_cur_channel(void);
const uint8_t*     beacon_svc_cur_bssid(void);
int                beacon_svc_burst_left(void);

#endif /* BS_HAS_WIFI */
