#pragma once
/*
 * wifi_eapol_svc.h - WPA2 EAPOL handshake capture service.
 *
 * Passively monitors 802.11 traffic for EAPOL frames (IEEE 802.1X,
 * ethertype 0x888E over LLC/SNAP).  Optional deauth support is scoped to
 * the capture context: the service learns AP<->STA pairs from normal 802.11
 * traffic, then only injects against learned APs/pairs or an explicit BSSID.
 *
 * Captured frames are written to a pcap file (DLT_IEEE802_11=105), readable
 * by Wireshark/aircrack-ng/hcxtools.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"

typedef enum {
    EAPOL_DEAUTH_OFF = 0,
    EAPOL_DEAUTH_LEARNED_PAIRS,
    EAPOL_DEAUTH_BROADCAST_APS,
    EAPOL_DEAUTH_PAIRS_AND_BCAST,
} eapol_deauth_mode_t;

typedef struct {
    eapol_deauth_mode_t mode;       /* OFF / learned STA pairs / broadcast APs / both */
    uint32_t            interval_ms;/* ms between bursts; 0 = default                 */
    uint8_t             burst;      /* repeats per target per interval; 0 = default   */
    uint16_t            reason;     /* 802.11 reason code; 0 = default 7              */
    bool                bidir;      /* forge AP->STA and STA->AP for learned pairs    */
    bool                disassoc;   /* send disassoc along with deauth                */
} eapol_deauth_cfg_t;

void eapol_svc_init(const bs_arch_t* arch);

/* Backward-compatible wrapper used by CLI code.
 * channel      0 = auto-hop 1-13; >0 = fixed channel
 * bssid        NULL = all APs; non-NULL = filter to this BSSID
 * do_deauth    legacy broadcast-from-BSSID mode; ignored without bssid
 * deauth_ivl   ms between deauth bursts; 0 = default 5000 ms
 * pcap_path    NULL = no pcap; non-NULL = write to this SD path
 */
bool eapol_svc_start(uint8_t channel, const uint8_t* bssid,
                     bool do_deauth, uint32_t deauth_ivl,
                     const char* pcap_path);

/* New configurable start used by the UI deauth submenu. */
bool eapol_svc_start_cfg(uint8_t channel, const uint8_t* bssid,
                         const eapol_deauth_cfg_t* deauth_cfg,
                         const char* pcap_path);

void eapol_svc_stop(void);
void eapol_svc_tick(uint32_t now_ms);
bool eapol_svc_active(void);

uint32_t eapol_svc_eapol_count(void);   /* total EAPOL frames captured          */
int      eapol_svc_pair_count(void);    /* completed BSSID+STA EAPOL pairs      */
int      eapol_svc_learned_count(void); /* learned AP<->STA traffic pairs       */
int      eapol_svc_ap_count(void);      /* learned AP/BSSID count               */
uint32_t eapol_svc_pcap_frames(void);   /* frames written to pcap               */
uint32_t eapol_svc_deauth_frames(void); /* forged deauth/disassoc frames sent   */

#endif /* BS_HAS_WIFI */
