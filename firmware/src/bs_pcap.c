/*
 * bs_pcap.c - libpcap file writer backed by bs_fs.
 *
 * No compile-time guard needed — this file is always compiled.
 * When bs_fs is unavailable (storage not mounted), bs_pcap_open()
 * returns NULL and all subsequent calls are safe no-ops.
 *
 * pcap file format:
 *   [Global header 24 B] [Record header 16 B | Frame data] ...
 *
 * The global header is written once at open; record headers are
 * written inline before each frame.  No in-memory buffering beyond
 * what bs_fs provides.
 */
#include "bs/bs_pcap.h"
#include "bs/bs_fs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── pcap on-disk structures (little-endian, packed) ────────────────────── */

typedef struct {
    uint32_t magic;         /* 0xa1b2c3d4 */
    uint16_t ver_major;     /* 2           */
    uint16_t ver_minor;     /* 4           */
    int32_t  thiszone;      /* 0           */
    uint32_t sigfigs;       /* 0           */
    uint32_t snaplen;       /* 65535       */
    uint32_t network;       /* link type   */
} pcap_global_hdr_t;

typedef struct {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;      /* bytes stored */
    uint32_t orig_len;      /* original length */
} pcap_rec_hdr_t;

/* ── Handle ──────────────────────────────────────────────────────────────── */

struct bs_pcap {
    bs_file_t  file;        /* void* handle returned by bs_fs_open */
    uint32_t   frame_count;
};

/* ── Internal helpers ────────────────────────────────────────────────────── */

static int write_global_header(bs_file_t* f, uint32_t link_type) {
    pcap_global_hdr_t h;
    h.magic     = 0xa1b2c3d4u;
    h.ver_major = 2;
    h.ver_minor = 4;
    h.thiszone  = 0;
    h.sigfigs   = 0;
    h.snaplen   = 65535;
    h.network   = link_type;
    return bs_fs_write(f, &h, sizeof h) == (int)sizeof h ? 0 : -1;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bs_pcap_t* bs_pcap_open_lt(const char* path, uint32_t link_type) {
    if (!path) return NULL;

    bs_file_t* f = bs_fs_open(path, "wb");
    if (!f) return NULL;

    if (write_global_header(f, link_type) != 0) {
        bs_fs_close(f);
        return NULL;
    }

    bs_pcap_t* p = (bs_pcap_t*)malloc(sizeof(bs_pcap_t));
    if (!p) { bs_fs_close(f); return NULL; }

    p->file        = f;
    p->frame_count = 0;
    return p;
}

bs_pcap_t* bs_pcap_open(const char* path) {
    return bs_pcap_open_lt(path, BS_PCAP_LT_IEEE802_11);
}

void bs_pcap_close(bs_pcap_t* p) {
    if (!p) return;
    bs_fs_close(p->file);
    free(p);
}

int bs_pcap_write(bs_pcap_t* p,
                  const uint8_t* frame, uint16_t len,
                  uint32_t ts_sec, uint32_t ts_usec) {
    if (!p || !frame || !len) return -1;

    pcap_rec_hdr_t h;
    h.ts_sec   = ts_sec;
    h.ts_usec  = ts_usec;
    h.incl_len = len;
    h.orig_len = len;

    if (bs_fs_write(p->file, &h, sizeof h) != (int)sizeof h) return -1;
    if (bs_fs_write(p->file, frame, len)   != (int)len)      return -1;

    p->frame_count++;
    return 0;
}

int bs_pcap_flush(bs_pcap_t* p) {
    /* bs_fs has no explicit flush — data is committed on close.
     * This is a best-effort call; callers may rely on bs_pcap_close()
     * for durability. */
    (void)p;
    return 0;
}

uint32_t bs_pcap_frame_count(const bs_pcap_t* p) {
    return p ? p->frame_count : 0;
}
