#pragma once
/*
 * bs_wifi.h - BeamStalker WiFi abstraction.
 *
 * Backends (select exactly one via build flags):
 *   BS_WIFI_ESP32   - ESP32/S3 built-in radio (CONNECT + SCAN + SNIFF + INJECT)
 *   BS_WIFI_NRF7002 - nRF7002 companion IC    (CONNECT + SCAN)
 *   BS_WIFI_STM     - STM32 + WiFi module     (CONNECT + SCAN)
 *   (none defined)  - silent no-ops; bs_wifi_caps() returns 0
 *
 * Always query bs_wifi_caps() before calling a capability-specific function;
 * unsupported calls return -1 immediately without side effects.
 *
 * State machine:
 *
 *   OFF → IDLE          bs_wifi_init()
 *   IDLE → SCANNING     bs_wifi_scan_start()
 *   SCANNING → IDLE     poll bs_wifi_scan_done(), read bs_wifi_scan_results()
 *   IDLE → CONNECTING   bs_wifi_connect()
 *   CONNECTING→CONNECTED poll bs_wifi_state() == BS_WIFI_STATE_CONNECTED
 *   CONNECTED → IDLE    bs_wifi_disconnect()
 *   * → MONITOR         bs_wifi_monitor_start()  — implicitly disconnects
 *   * → SNIFF           bs_wifi_sniff_start()    — implicitly disconnects
 *   MONITOR/SNIFF→IDLE  bs_wifi_monitor_stop() / bs_wifi_sniff_stop()
 *   IDLE → OFF          bs_wifi_deinit()
 *
 * Thread safety: not guaranteed. Call from one task/thread only, or add
 * external locking.  Frame callbacks may fire from ISR context on ESP32.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bs_arch.h"

/* ── Convenience: defined when any real WiFi backend is active ─────────── */

#if defined(BS_WIFI_ESP32) || defined(BS_WIFI_NRF7002) || defined(BS_WIFI_STM)
#  define BS_HAS_WIFI 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capability flags ──────────────────────────────────────────────────── */

#define BS_WIFI_CAP_CONNECT  (1u << 0)  /* STA association / WPA2          */
#define BS_WIFI_CAP_SCAN     (1u << 1)  /* active + passive AP discovery   */
#define BS_WIFI_CAP_SNIFF    (1u << 2)  /* promiscuous 802.11 frame rx      */
#define BS_WIFI_CAP_MONITOR  (1u << 3)  /* monitor mode with radiotap hdr  */
#define BS_WIFI_CAP_INJECT   (1u << 4)  /* raw 802.11 frame transmit        */

/* ── Driver state ──────────────────────────────────────────────────────── */

typedef enum {
    BS_WIFI_STATE_OFF        = 0,   /* driver not initialised             */
    BS_WIFI_STATE_IDLE,             /* idle; no connection, scan or sniff */
    BS_WIFI_STATE_SCANNING,         /* async scan in progress             */
    BS_WIFI_STATE_CONNECTING,       /* STA association in progress        */
    BS_WIFI_STATE_CONNECTED,        /* STA associated, IP assigned        */
    BS_WIFI_STATE_MONITOR,          /* monitor / sniff mode active        */
} bs_wifi_state_t;

/* ── Authentication modes ──────────────────────────────────────────────── */

typedef enum {
    BS_WIFI_AUTH_OPEN         = 0,
    BS_WIFI_AUTH_WEP,
    BS_WIFI_AUTH_WPA_PSK,
    BS_WIFI_AUTH_WPA2_PSK,
    BS_WIFI_AUTH_WPA_WPA2_PSK,
    BS_WIFI_AUTH_WPA3_PSK,
    BS_WIFI_AUTH_UNKNOWN      = 0xFF,
} bs_wifi_auth_t;

/* ── AP scan result ────────────────────────────────────────────────────── */

typedef struct {
    char           ssid[33];    /* null-terminated; empty = hidden SSID   */
    uint8_t        bssid[6];    /* MAC address                             */
    int8_t         rssi;        /* dBm                                     */
    uint8_t        channel;     /* 1–14                                    */
    bs_wifi_auth_t auth;
} bs_wifi_ap_t;

/* ── Raw frame callback ────────────────────────────────────────────────── */

/*
 * Invoked for each received frame when in monitor or sniff mode.
 *
 *   frame - raw 802.11 MAC PDU (FCS stripped); in monitor mode some
 *           backends prepend a radiotap header — check BS_WIFI_CAP_MONITOR.
 *   len   - byte length of frame.
 *   rssi  - signal level in dBm (0 if unavailable).
 *   ctx   - opaque pointer supplied to monitor/sniff_start().
 *
 * MAY be called from ISR context (ESP32).  Keep it short; copy data out
 * rather than processing in place.  Do not call bs_wifi_* from inside.
 */
typedef void (*bs_wifi_frame_cb_t)(const uint8_t* frame, uint16_t len,
                                   int8_t rssi, void* ctx);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Power up radio, allocate driver resources.  Idempotent.
 * Returns 0 on success, <0 on hardware error.                              */
int  bs_wifi_init(const bs_arch_t* arch);

/* Power down radio, stop all activity, free resources.                     */
void bs_wifi_deinit(void);

/* Bitmask of BS_WIFI_CAP_* flags supported by this backend + hardware.    */
uint32_t bs_wifi_caps(void);

/* Current driver state.  On platforms with async connect this also advances
 * CONNECTING → CONNECTED/IDLE by polling the underlying stack.             */
bs_wifi_state_t bs_wifi_state(void);

/* ── Scan ──────────────────────────────────────────────────────────────── */

/* Kick off an async channel scan.
 * Returns 0 if started, -1 if unsupported, -2 if state prevents it.       */
int  bs_wifi_scan_start(void);

/* Non-blocking: returns true once the last scan has completed.             */
bool bs_wifi_scan_done(void);

/* Copy up to max_count results into out[].  Safe to call before scan_done
 * (returns 0 partial results).
 * Returns number of APs written, or <0 on error.                          */
int  bs_wifi_scan_results(bs_wifi_ap_t* out, int max_count);

/* ── Connect ───────────────────────────────────────────────────────────── */

/* Begin async STA connection.  Poll bs_wifi_state() for the outcome.
 * Returns 0 if attempt started, <0 if unsupported or bad state.            */
int  bs_wifi_connect(const char* ssid, const char* password);

/* Drop current association; return to IDLE.                                */
void bs_wifi_disconnect(void);

/* Write assigned IPv4 address as a dotted-decimal string (needs ≥16 bytes).
 * Returns 0 when connected and IP is available, <0 otherwise.              */
int  bs_wifi_get_ip(char* buf, size_t len);

/* ── Monitor mode (802.11 + optional radiotap) ─────────────────────────── */

/* Enter full monitor mode on channel.  All 802.11 frame types are received.
 * On backends that support BS_WIFI_CAP_MONITOR the frame pointer includes a
 * radiotap header; on backends that only have BS_WIFI_CAP_SNIFF the raw
 * 802.11 MAC PDU is delivered with no radiotap prefix.
 * Returns 0 on success, -1 if unsupported, -2 on driver error.            */
int  bs_wifi_monitor_start(uint8_t channel, bs_wifi_frame_cb_t cb, void* ctx);
void bs_wifi_monitor_stop(void);

/* ── Promiscuous sniff (data frames only) ──────────────────────────────── */

/* Enter promiscuous mode on channel, delivering data frames only.
 * Lighter-weight than full monitor; available on more hardware.
 * Returns 0 on success, -1 if unsupported, -2 on driver error.            */
int  bs_wifi_sniff_start(uint8_t channel, bs_wifi_frame_cb_t cb, void* ctx);
void bs_wifi_sniff_stop(void);

/* Change the active channel while monitor or sniff is running.
 * Returns 0 on success, <0 on error.                                       */
int  bs_wifi_set_channel(uint8_t channel);

/* ── TX power ───────────────────────────────────────────────────────────── */

/* Set transmit power in dBm.  Clamped to hardware limits by the backend.
 * Typical ESP32 range: 2–20 dBm.  Use 20 for maximum range.
 * Returns 0 on success, <0 if unsupported or out of range.                */
int bs_wifi_set_tx_power(int dbm);

/* ── Raw frame inject ──────────────────────────────────────────────────── */

/* Which logical WiFi interface to transmit on.
 *
 *   BS_WIFI_IF_STA  — station interface (default, always available)
 *   BS_WIFI_IF_AP   — access-point interface.  Required when spoofing AP
 *                     role (e.g. deauth frames that appear to come from the
 *                     AP).  The backend switches to APSTA mode internally
 *                     if needed; the caller does not need to manage modes.
 */
typedef enum {
    BS_WIFI_IF_STA = 0,
    BS_WIFI_IF_AP,
} bs_wifi_if_t;

/* Transmit a complete 802.11 MAC PDU (no FCS — driver appends it).
 * Returns 0 on success, -1 if unsupported, -2 on driver error.            */
int  bs_wifi_send_raw(bs_wifi_if_t iface, const uint8_t* frame, uint16_t len);

/* ── Utilities (always available, no hardware needed) ──────────────────── */

/* Format BSSID bytes as "aa:bb:cc:dd:ee:ff".  buf must be ≥18 bytes.      */
void bs_wifi_bssid_str(const uint8_t bssid[6], char* buf18);

/* Short human-readable auth mode string ("OPEN", "WPA2", …).              */
const char* bs_wifi_auth_str(bs_wifi_auth_t auth);

#ifdef __cplusplus
}
#endif
