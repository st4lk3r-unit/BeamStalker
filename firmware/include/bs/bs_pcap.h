#pragma once
/*
 * bs_pcap.h - Platform-agnostic libpcap file writer.
 *
 * Writes Wireshark-compatible .pcap files to any path accessible through
 * bs_fs.  No radio or hardware dependency — works equally with WiFi frames,
 * BLE packets, or any other byte stream the caller supplies.
 *
 * File format: standard libpcap (magic 0xa1b2c3d4, link type 105 = IEEE 802.11
 * by default; override with bs_pcap_open_lt() for other link types).
 *
 * Usage:
 *   bs_pcap_t* p = bs_pcap_open("captures/session.pcap");
 *   bs_pcap_write(p, frame, len, ts_sec, ts_usec);
 *   bs_pcap_close(p);
 *
 * Common link types:
 *   BS_PCAP_LT_IEEE802_11  (105) — raw 802.11 frames (WiFi sniffer)
 *   BS_PCAP_LT_BLUETOOTH_LE_LL (251) — BLE link-layer frames
 *   BS_PCAP_LT_ETHERNET    (1)  — Ethernet II frames
 *   BS_PCAP_LT_RADIOTAP    (127) — 802.11 with radiotap header
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Link-type constants ────────────────────────────────────────────────── */

#define BS_PCAP_LT_ETHERNET        1
#define BS_PCAP_LT_IEEE802_11    105
#define BS_PCAP_LT_RADIOTAP      127
#define BS_PCAP_LT_BLUETOOTH_LE  251

/* ── Writer handle ──────────────────────────────────────────────────────── */

typedef struct bs_pcap bs_pcap_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* Open (or create) a pcap file at path and write the global header.
 * Link type defaults to BS_PCAP_LT_IEEE802_11.
 * Returns NULL on failure (file could not be opened / storage not available). */
bs_pcap_t* bs_pcap_open(const char* path);

/* Same as bs_pcap_open() but with an explicit link type constant.          */
bs_pcap_t* bs_pcap_open_lt(const char* path, uint32_t link_type);

/* Flush and close the file.  The handle is invalid after this call.        */
void bs_pcap_close(bs_pcap_t* p);

/* ── Writing ────────────────────────────────────────────────────────────── */

/* Append one frame to the pcap file.
 *   frame    - raw bytes of the captured frame (no FCS).
 *   len      - number of bytes in frame[].
 *   ts_sec   - capture timestamp, seconds since Unix epoch.
 *   ts_usec  - capture timestamp, microseconds fraction.
 *
 * Returns 0 on success, <0 on write error.                                 */
int bs_pcap_write(bs_pcap_t* p,
                  const uint8_t* frame, uint16_t len,
                  uint32_t ts_sec, uint32_t ts_usec);

/* Flush buffered data to storage.  Called automatically by bs_pcap_close().
 * Returns 0 on success, <0 on error.                                       */
int bs_pcap_flush(bs_pcap_t* p);

/* Total number of frames written to this file so far.                      */
uint32_t bs_pcap_frame_count(const bs_pcap_t* p);

#ifdef __cplusplus
}
#endif
