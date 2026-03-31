/*
 * bs_wifi_none.c - No-op WiFi backend.
 *
 * Activated when no real WiFi backend is selected (no BS_WIFI_* flag
 * defined).  All functions return immediately; bs_wifi_caps() returns 0 so
 * callers know the radio is absent without needing separate compile-time
 * guards in application code.
 */
#if !defined(BS_WIFI_ESP32) && !defined(BS_WIFI_NRF7002) && !defined(BS_WIFI_STM)

#include "bs/bs_wifi.h"

int             bs_wifi_init(const bs_arch_t* a)  { (void)a; return 0;              }
void            bs_wifi_deinit(void)               {                                  }
uint32_t        bs_wifi_caps(void)                 { return 0;                       }
bs_wifi_state_t bs_wifi_state(void)                { return BS_WIFI_STATE_OFF;       }

int  bs_wifi_scan_start(void)                      { return -1;                      }
bool bs_wifi_scan_done(void)                       { return false;                   }
int  bs_wifi_scan_results(bs_wifi_ap_t* o, int n)  { (void)o; (void)n; return -1;   }

int  bs_wifi_connect(const char* s, const char* p) { (void)s; (void)p; return -1;   }
void bs_wifi_disconnect(void)                      {                                  }
int  bs_wifi_get_ip(char* b, size_t l)             { (void)b; (void)l; return -1;   }

int  bs_wifi_monitor_start(uint8_t ch, bs_wifi_frame_cb_t cb, void* ctx)
     { (void)ch; (void)cb; (void)ctx; return -1; }
void bs_wifi_monitor_stop(void)                    {                                  }

int  bs_wifi_sniff_start(uint8_t ch, bs_wifi_frame_cb_t cb, void* ctx)
     { (void)ch; (void)cb; (void)ctx; return -1; }
void bs_wifi_sniff_stop(void)                      {                                  }

int  bs_wifi_set_tx_power(int dbm)                 { (void)dbm; return -1;           }
int  bs_wifi_set_channel(uint8_t ch)               { (void)ch; return -1;            }
int  bs_wifi_send_raw(bs_wifi_if_t iface, const uint8_t* f, uint16_t l)
     { (void)iface; (void)f; (void)l; return -1; }

#endif /* no backend */
