#pragma once
/*
 * wifi_eapol_svc.h - WPA2 EAPOL handshake capture service.
 *
 * Passively monitors 802.11 traffic for EAPOL frames (IEEE 802.1X,
 * ethertype 0x88-8E over LLC/SNAP).  When do_deauth is set, periodically
 * injects broadcast deauth frames spoofed from the target BSSID to force
 * client reauthentication and trigger a fresh 4-way handshake.
 *
 * Captured frames are written to a pcap file (DLT_IEEE802_11=105), readable
 * by hashcat (-m 22000), aircrack-ng, or hcxtools for offline WPA2 cracking.
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include <stdint.h>
#include <stdbool.h>
#include "bs/bs_arch.h"

void eapol_svc_init(const bs_arch_t* arch);

/*
 * channel      0 = auto-hop 1-13; >0 = fixed channel
 * bssid        NULL = all APs; non-NULL = filter to this BSSID
 * do_deauth    periodically inject broadcast deauth from bssid (requires bssid)
 * deauth_ivl   ms between deauth bursts; 0 = default 5000 ms
 * pcap_path    NULL = no pcap; non-NULL = write to this SD path
 */
bool eapol_svc_start(uint8_t channel, const uint8_t* bssid,
                     bool do_deauth, uint32_t deauth_ivl,
                     const char* pcap_path);
void eapol_svc_stop(void);
void eapol_svc_tick(uint32_t now_ms);
bool eapol_svc_active(void);

uint32_t eapol_svc_eapol_count(void);  /* total EAPOL frames captured        */
int      eapol_svc_pair_count(void);   /* BSSID+STA pairs with both M1 + M2  */
uint32_t eapol_svc_pcap_frames(void);  /* frames written to pcap              */

#endif /* BS_HAS_WIFI */
