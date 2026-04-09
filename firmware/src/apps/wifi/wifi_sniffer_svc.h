#pragma once
/*
 * wifi_sniffer_svc.h - WiFi sniffer service layer.
 *
 * The service owns the promiscuous callback and ring buffer.
 * Callers register a packet handler via sniffer_svc_start(); the handler
 * is called from the main loop (sniffer_svc_tick()) — not from ISR.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"

typedef enum {
    SNIFFER_SVC_IDLE = 0,
    SNIFFER_SVC_RUNNING,
} sniffer_svc_state_t;

/* Packet handler called from sniffer_svc_tick() (main loop context). */
typedef void (*sniffer_svc_pkt_fn)(const uint8_t* data, uint16_t len,
                                   int8_t rssi, uint32_t ts_ms, void* ctx);

void sniffer_svc_init(const bs_arch_t* arch);
/* channel=0: auto-hop every hop_ms; channel>0: fixed */
void sniffer_svc_start(uint8_t channel, uint32_t hop_ms,
                       sniffer_svc_pkt_fn cb, void* ctx);
void sniffer_svc_stop(void);
/* Drain ring buffer → calls cb for each queued frame. */
void sniffer_svc_tick(uint32_t now_ms);
/* Manually change channel while running (no-op if fixed). */
void sniffer_svc_set_channel(uint8_t ch);

sniffer_svc_state_t sniffer_svc_state(void);
uint8_t             sniffer_svc_channel(void);
uint32_t            sniffer_svc_count(void);
uint32_t            sniffer_svc_dropped(void);

#endif /* BS_HAS_WIFI */
