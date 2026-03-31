/*
 * wifi_sniffer.c - WiFi sniffer sub-application.
 *
 * Features:
 *   - Channel: fixed (1-13) or auto-hop every 500 ms
 *   - Frame filter: type + subtype based (12 presets)
 *   - MAC filter: pin to Addr1/Addr2/Addr3 of selected packet
 *   - PCAP output to /pcap/wifi_NNNN.pcap on SD
 *   - SPSC ring buffer for safe WiFi-callback → main-loop handoff
 *   - Scrollable, selectable packet list (16 entries)
 *   - Packet expand: full Addr1/Addr2/Addr3 + details
 *   - Text carousel for long rows in the compact list
 *
 * 802.11 Frame Control byte 0:
 *   bits 3:2 = type:  0=MGMT  1=CTRL  2=DATA  3=EXT
 *   bits 7:4 = subtype
 *
 * Navigation (MENU):
 *   UP/DOWN      - move cursor
 *   LEFT/RIGHT   - cycle filter / adjust channel
 *   SELECT       - pick item / start
 *   BACK         - exit
 *
 * Navigation (RUNNING, not expanded):
 *   UP/DOWN      - move packet cursor (UP = older, DOWN = newer)
 *   LEFT/RIGHT   - cycle frame filter
 *   SELECT       - expand selected packet
 *   BACK         - stop, return to menu
 *
 * Navigation (RUNNING, expanded):
 *   LEFT/RIGHT   - cycle MAC filter: NONE → Addr2(src) → Addr1(dst) → Addr3(bssid)
 *   SELECT/BACK  - close expanded view
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_sniffer.h"
#include "wifi_common.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"
#include "bs/bs_fs.h"
#include "bs/bs_pcap.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define RING_SLOTS      32
#define RING_FRAME_MAX 512

typedef struct {
    uint8_t  data[RING_FRAME_MAX];
    uint16_t len;
    int8_t   rssi;
    uint32_t ts_ms;
} ring_slot_t;

static ring_slot_t          s_ring[RING_SLOTS];
static volatile atomic_uint s_ring_head;
static volatile atomic_uint s_ring_tail;
static uint32_t             s_ring_dropped;

static bool ring_push(const uint8_t* frame, uint16_t len, int8_t rssi,
                      uint32_t ts_ms) {
    if (len > RING_FRAME_MAX) { s_ring_dropped++; return false; }
    unsigned head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
    unsigned next = (head + 1) % RING_SLOTS;
    unsigned tail = atomic_load_explicit(&s_ring_tail, memory_order_acquire);
    if (next == tail) { s_ring_dropped++; return false; }
    ring_slot_t* slot = &s_ring[head];
    memcpy(slot->data, frame, len);
    slot->len   = len;
    slot->rssi  = rssi;
    slot->ts_ms = ts_ms;
    atomic_store_explicit(&s_ring_head, next, memory_order_release);
    return true;
}

static bool ring_pop(ring_slot_t* out) {
    unsigned tail = atomic_load_explicit(&s_ring_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&s_ring_head, memory_order_acquire);
    if (tail == head) return false;
    *out = s_ring[tail];
    atomic_store_explicit(&s_ring_tail,
                          (tail + 1) % RING_SLOTS,
                          memory_order_release);
    return true;
}

/* ── 802.11 FC helpers ───────────────────────────────────────────────────── */

static const char* fc_type_str(uint8_t type) {
    switch (type & 0x3) {
        case 0: return "MGMT";
        case 1: return "CTRL";
        case 2: return "DATA";
        case 3: return "EXT";
    }
    return "?";
}

static const char* fc_subtype_str(uint8_t type, uint8_t subtype) {
    subtype &= 0xF;
    switch (type & 0x3) {
        case 0: {
            static const char* const m[16] = {
                "AssocReq", "AssocRsp", "ReassReq",  "ReassRsp",
                "PrbReq",   "PrbRsp",   "TimAdv",    "Rsvd",
                "Beacon",   "ATIM",     "Disassoc",  "Auth",
                "Deauth",   "Action",   "ActNoAck",  "Rsvd"
            };
            return m[subtype];
        }
        case 1: {
            static const char* const c[16] = {
                "Rsvd",    "Rsvd",   "Trigger", "TACK",
                "BFPoll",  "NDP",    "CtrlExt", "CtrlWrap",
                "BAR",     "BA",     "PS-Poll", "RTS",
                "CTS",     "ACK",    "CF-End",  "CF-End+ACK"
            };
            return c[subtype];
        }
        case 2: {
            static const char* const d[16] = {
                "Data",     "Rsvd",    "Rsvd",     "Rsvd",
                "Null",     "Rsvd",    "Rsvd",     "Rsvd",
                "QoS",      "QoS+ACK", "QoS+Poll", "QoS+A+P",
                "QoSNull",  "Rsvd",    "QosCFPoll","QosCF+ACK"
            };
            return d[subtype];
        }
        case 3:
            if (subtype == 0) return "DMGBeacon";
            if (subtype == 1) return "S1GBeacon";
            return "Rsvd";
    }
    return "?";
}

/* ── Filter ──────────────────────────────────────────────────────────────── */

typedef enum {
    SNF_FILTER_ALL = 0,   /* every 802.11 frame        */
    SNF_FILTER_MGMT,      /* type 0 — all management   */
    SNF_FILTER_CTRL,      /* type 1 — all control      */
    SNF_FILTER_DATA,      /* type 2 — all data         */
    SNF_FILTER_BEACON,    /* MGMT/8                    */
    SNF_FILTER_PROBE_REQ, /* MGMT/4                    */
    SNF_FILTER_PROBE_RSP, /* MGMT/5                    */
    SNF_FILTER_AUTH,      /* MGMT/11                   */
    SNF_FILTER_DEAUTH,    /* MGMT/12                   */
    SNF_FILTER_ASSOC,     /* MGMT/0,1                  */
    SNF_FILTER_DISASSOC,  /* MGMT/10                   */
    SNF_FILTER_QOS,       /* DATA subtype 8-15         */
    SNF_FILTER_COUNT
} snf_filter_t;

/* Array must be index-aligned with the enum above */
static const char* const k_filter_names[SNF_FILTER_COUNT] = {
    "ALL", "MGMT", "CTRL", "DATA",
    "BEACON", "PROBE-REQ", "PROBE-RSP",
    "AUTH", "DEAUTH", "ASSOC", "DISASSOC", "QoS"
};

/* ── MAC filter ──────────────────────────────────────────────────────────── */

typedef enum {
    MAC_FILTER_NONE  = 0,
    MAC_FILTER_ADDR2,   /* Addr2: source/transmitter  */
    MAC_FILTER_ADDR1,   /* Addr1: destination/receiver */
    MAC_FILTER_ADDR3,   /* Addr3: BSSID               */
    MAC_FILTER_COUNT
} mac_filter_mode_t;

static const char* const k_mac_filter_names[MAC_FILTER_COUNT] = {
    "OFF", "src", "dst", "bssid"
};

/* ── State ───────────────────────────────────────────────────────────────── */

typedef enum { SNF_MENU, SNF_RUNNING } snf_state_t;
typedef enum { SNF_CH_AUTO, SNF_CH_FIXED } snf_chmode_t;

static snf_state_t      s_state;
static snf_filter_t     s_filter;
static snf_chmode_t     s_chmode;
static uint8_t          s_channel;
static int              s_menu_cursor;
static bool             s_dirty;

/* Runtime */
static wifi_pps_t       s_pps;
static uint32_t         s_total_bytes;
static uint32_t         s_hop_ms;
static bs_pcap_t*       s_pcap;
static char             s_pcap_path[32];
static int              s_pkt_cursor;     /* selected packet: 0=newest */
static bool             s_pkt_expanded;

/* MAC filter */
static mac_filter_mode_t s_mac_filter_mode;
static uint8_t           s_mac_filter_addr[6];

/* Carousel (highlighted row only) */
#define CAROUSEL_INTERVAL_MS  220
static uint32_t         s_carousel_ms;
static int              s_carousel_offset;

static volatile uint32_t s_cb_millis;

/* ── Display frame ring ──────────────────────────────────────────────────── */

#define DISP_MAX  16

typedef struct {
    char     label[16];  /* "MGMT:Beacon" */
    int8_t   rssi;
    uint16_t len;
    uint8_t  addr1[6];   /* Addr1: destination/receiver */
    uint8_t  addr2[6];   /* Addr2: source/transmitter   */
    uint8_t  addr3[6];   /* Addr3: BSSID (MGMT/DATA)    */
    bool     has_addr;   /* addr1+addr2 valid (len >= 16) */
    bool     has_addr3;  /* addr3 valid (MGMT/DATA, len >= 22) */
} disp_frame_t;

static disp_frame_t s_disp[DISP_MAX];
static int          s_disp_head;
static int          s_disp_count;
static disp_frame_t s_pkt_pinned;  /* snapshot of packet when expanded      */

static void disp_reset(void) {
    s_disp_head  = 0;
    s_disp_count = 0;
    s_pkt_cursor = 0;
}

static void disp_push(const ring_slot_t* slot) {
    disp_frame_t* d = &s_disp[s_disp_head];
    uint8_t fc0     = slot->len >= 1 ? slot->data[0] : 0;
    uint8_t type    = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    snprintf(d->label, sizeof d->label, "%s:%s",
             fc_type_str(type), fc_subtype_str(type, subtype));
    d->rssi = slot->rssi;
    d->len  = slot->len;

    if (slot->len >= 16) {
        memcpy(d->addr1, slot->data + 4,  6);   /* Addr1 */
        memcpy(d->addr2, slot->data + 10, 6);   /* Addr2 */
        d->has_addr = true;
    } else {
        d->has_addr = false;
    }

    if (slot->len >= 22 && (type == 0 || type == 2)) {
        memcpy(d->addr3, slot->data + 16, 6);   /* Addr3 */
        d->has_addr3 = true;
    } else {
        d->has_addr3 = false;
    }

    s_disp_head = (s_disp_head + 1) % DISP_MAX;
    if (s_disp_count < DISP_MAX) s_disp_count++;
}

/* ── Frame filter ────────────────────────────────────────────────────────── */

static bool passes_filter(const uint8_t* data, uint16_t len) {
    if (len < 2) return false;
    uint8_t fc0     = data[0];
    uint8_t type    = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    bool type_ok;
    switch (s_filter) {
        case SNF_FILTER_ALL:       type_ok = true; break;
        case SNF_FILTER_MGMT:      type_ok = (type == 0); break;
        case SNF_FILTER_CTRL:      type_ok = (type == 1); break;
        case SNF_FILTER_DATA:      type_ok = (type == 2); break;
        case SNF_FILTER_BEACON:    type_ok = (type == 0 && subtype == 8);  break;
        case SNF_FILTER_PROBE_REQ: type_ok = (type == 0 && subtype == 4);  break;
        case SNF_FILTER_PROBE_RSP: type_ok = (type == 0 && subtype == 5);  break;
        case SNF_FILTER_AUTH:      type_ok = (type == 0 && subtype == 11); break;
        case SNF_FILTER_DEAUTH:    type_ok = (type == 0 && subtype == 12); break;
        case SNF_FILTER_ASSOC:     type_ok = (type == 0 && (subtype == 0 || subtype == 1)); break;
        case SNF_FILTER_DISASSOC:  type_ok = (type == 0 && subtype == 10); break;
        case SNF_FILTER_QOS:       type_ok = (type == 2 && subtype >= 8);  break;
        default:                   type_ok = true; break;
    }
    if (!type_ok) return false;

    /* Optional MAC filter */
    if (s_mac_filter_mode != MAC_FILTER_NONE && len >= 16) {
        const uint8_t* addr = NULL;
        switch (s_mac_filter_mode) {
            case MAC_FILTER_ADDR2: addr = data + 10; break;  /* src  */
            case MAC_FILTER_ADDR1: addr = data + 4;  break;  /* dst  */
            case MAC_FILTER_ADDR3:
                if (len >= 22) addr = data + 16;             /* bssid */
                break;
            default: break;
        }
        if (addr) return memcmp(addr, s_mac_filter_addr, 6) == 0;
    }
    return true;
}

/* ── Frame callback ──────────────────────────────────────────────────────── */

static void frame_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)ctx;
    ring_push(frame, len, rssi, s_cb_millis);
}

/* ── PCAP ────────────────────────────────────────────────────────────────── */

static void pcap_open_next(void) {
    if (!bs_fs_available()) { s_pcap = NULL; return; }
    bs_fs_mkdir_p("/pcap");
    for (int i = 0; i < 9999; i++) {
        snprintf(s_pcap_path, sizeof s_pcap_path, "/pcap/wifi_%04d.pcap", i);
        if (!bs_fs_exists(s_pcap_path)) {
            s_pcap = bs_pcap_open(s_pcap_path);
            return;
        }
    }
    s_pcap = NULL;
}

/* ── Channel hop ─────────────────────────────────────────────────────────── */

static void hop_channel(uint32_t now_ms) {
    if (s_chmode != SNF_CH_AUTO) return;
    if (now_ms - s_hop_ms < 500) return;
    s_hop_ms = now_ms;
    s_channel = (s_channel % 13) + 1;
    bs_wifi_set_channel(s_channel);
}

/* ── Start / stop ────────────────────────────────────────────────────────── */

static void start_sniff(const bs_arch_t* arch) {
    (void)arch;
    atomic_init(&s_ring_head, 0);
    atomic_init(&s_ring_tail, 0);
    s_ring_dropped    = 0;
    s_pkt_expanded    = false;
    s_carousel_offset = 0;
    s_carousel_ms     = 0;
    wifi_pps_init(&s_pps);
    s_total_bytes = 0;
    s_hop_ms      = 0;
    disp_reset();

    pcap_open_next();
    bs_wifi_monitor_start(s_channel, frame_cb, NULL);

    s_state = SNF_RUNNING;
    s_dirty = true;
}

static void stop_sniff(void) {
    bs_wifi_monitor_stop();
    if (s_pcap) { bs_pcap_close(s_pcap); s_pcap = NULL; }
    s_pkt_expanded = false;
}

/* ── Helper: index of selected packet in disp ring ──────────────────────── */

static const disp_frame_t* disp_at_cursor(void) {
    if (s_disp_count == 0) return NULL;
    int idx = ((s_disp_head - 1 - s_pkt_cursor) % DISP_MAX + DISP_MAX) % DISP_MAX;
    return &s_disp[idx];
}

/* ── Draw: menu ──────────────────────────────────────────────────────────── */

static void draw_menu(void) {
    int ts = bs_ui_text_scale();
    int sw = bs_gfx_width();
    int cy = bs_ui_content_y();
    int lh = bs_gfx_text_h(ts) + 4;
    const char* chmodes[] = { "AUTO-HOP", "FIXED" };

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Sniffer");

    char filter_buf[32], chmode_buf[28], ch_buf[24];
    snprintf(filter_buf, sizeof filter_buf,
             "Filter:  %-10s", k_filter_names[s_filter]);
    snprintf(chmode_buf, sizeof chmode_buf,
             "Channel: %-8s", chmodes[s_chmode]);
    snprintf(ch_buf, sizeof ch_buf, "  Fixed ch: %d", s_channel);

    const char* items[5];
    items[0] = "Start";
    items[1] = filter_buf;
    items[2] = chmode_buf;
    items[3] = (s_chmode == SNF_CH_FIXED) ? ch_buf : NULL;
    items[4] = "Back";

    int hint_h  = bs_gfx_text_h(ts) + 6;
    int max_rows = (bs_gfx_height() - cy - hint_h) / lh;
    if (max_rows < 1) max_rows = 1;

    /* Scroll so cursor is always visible */
    int scroll = 0;
    if (s_menu_cursor >= max_rows)
        scroll = s_menu_cursor - max_rows + 1;

    int row = 0;   /* index into visible (non-NULL) items */
    int drawn = 0;
    for (int i = 0; i < 5; i++) {
        if (!items[i]) continue;
        /* sel uses row (visible index), not i (array index) — fixes Back
         * never being highlighted when item[3] is NULL (AUTO ch mode).     */
        if (row < scroll) { row++; continue; }
        if (drawn >= max_rows) break;
        bool sel = (row == s_menu_cursor);
        int  y   = cy + drawn * lh;
        if (sel) bs_gfx_fill_rect(0, y - 1, sw, lh - 1, g_bs_theme.dim);
        bs_color_t col = sel ? g_bs_theme.accent : g_bs_theme.primary;
        bs_gfx_text(8, y, items[i], col, ts);
        row++;
        drawn++;
    }

    bs_ui_draw_hint("<<>>:filter  SELECT=pick  BACK=exit");
    bs_gfx_present();
}

/* ── Draw: running ───────────────────────────────────────────────────────── */

static void draw_running(void) {
    int ts  = bs_ui_text_scale();
    int ts2 = ts > 1 ? ts - 1 : 1;
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;
    char buf[80];

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Sniffer [RUNNING]");

    /* ── Stats ── */
    if (s_mac_filter_mode != MAC_FILTER_NONE) {
        snprintf(buf, sizeof buf, "Ch:%d%s  %s  MAC[%s]:%02x:%02x:%02x:%02x:%02x:%02x",
                 s_channel, s_chmode == SNF_CH_AUTO ? "(hop)" : "",
                 k_filter_names[s_filter],
                 k_mac_filter_names[s_mac_filter_mode],
                 s_mac_filter_addr[0], s_mac_filter_addr[1],
                 s_mac_filter_addr[2], s_mac_filter_addr[3],
                 s_mac_filter_addr[4], s_mac_filter_addr[5]);
    } else {
        snprintf(buf, sizeof buf, "Ch:%d%s  Filter:%s",
                 s_channel, s_chmode == SNF_CH_AUTO ? "(hop)" : "",
                 k_filter_names[s_filter]);
    }
    bs_gfx_text(8, cy, buf, g_bs_theme.secondary, ts2);

    snprintf(buf, sizeof buf, "Frames:%lu (%luB)  PPS:%lu  Drop:%lu",
             (unsigned long)s_pps.total,   (unsigned long)s_total_bytes,
             (unsigned long)s_pps.pps,     (unsigned long)s_ring_dropped);
    bs_gfx_text(8, cy + lh2, buf, g_bs_theme.primary, ts2);

    /* ── Separator ── */
    int sep_y = cy + 2 * lh2 + 3;
    bs_gfx_fill_rect(0, sep_y, bs_gfx_width(), 1, g_bs_theme.dim);
    int list_y = sep_y + 3;

    /* ── Expanded view ── */
    if (s_pkt_expanded) {
        const disp_frame_t* d = &s_pkt_pinned;  /* pinned at SELECT time    */
        if (d) {
            int y = list_y;
            bs_gfx_text(8, y, d->label, g_bs_theme.accent, ts);
            y += lh;

            snprintf(buf, sizeof buf, "RSSI:%d  Len:%d",
                     (int)d->rssi, (int)d->len);
            bs_gfx_text(8, y, buf, g_bs_theme.primary, ts2);
            y += lh2;

            if (d->has_addr) {
                bool sel2 = (s_mac_filter_mode == MAC_FILTER_ADDR2);
                bool sel1 = (s_mac_filter_mode == MAC_FILTER_ADDR1);
                snprintf(buf, sizeof buf, "Src  %02x:%02x:%02x:%02x:%02x:%02x%s",
                         d->addr2[0], d->addr2[1], d->addr2[2],
                         d->addr2[3], d->addr2[4], d->addr2[5],
                         sel2 ? " *" : "");
                bs_gfx_text(8, y, buf, sel2 ? g_bs_theme.warn : g_bs_theme.primary, ts2);
                y += lh2;

                snprintf(buf, sizeof buf, "Dst  %02x:%02x:%02x:%02x:%02x:%02x%s",
                         d->addr1[0], d->addr1[1], d->addr1[2],
                         d->addr1[3], d->addr1[4], d->addr1[5],
                         sel1 ? " *" : "");
                bs_gfx_text(8, y, buf, sel1 ? g_bs_theme.warn : g_bs_theme.primary, ts2);
                y += lh2;
            }

            if (d->has_addr3) {
                bool sel3 = (s_mac_filter_mode == MAC_FILTER_ADDR3);
                snprintf(buf, sizeof buf, "BSSID%02x:%02x:%02x:%02x:%02x:%02x%s",
                         d->addr3[0], d->addr3[1], d->addr3[2],
                         d->addr3[3], d->addr3[4], d->addr3[5],
                         sel3 ? " *" : "");
                bs_gfx_text(8, y, buf, sel3 ? g_bs_theme.warn : g_bs_theme.primary, ts2);
            }
        } else {
            bs_gfx_text(8, list_y, "no packet", g_bs_theme.dim, ts);
        }

        bs_ui_draw_hint("<<>>:pin addr  SEL/BACK=close");
        bs_gfx_present();
        return;
    }

    /* ── Compact packet list ── */
    int max_y   = cy + bs_ui_content_h() - lh;
    int visible = (max_y - list_y) / lh2;
    if (visible < 1) visible = 1;
    if (visible > DISP_MAX) visible = DISP_MAX;

    int max_cursor = s_disp_count - 1;
    if (max_cursor < 0) max_cursor = 0;
    if (s_pkt_cursor > max_cursor) s_pkt_cursor = max_cursor;

    /* Compute scroll so cursor is visible */
    int scroll = 0;
    if (s_pkt_cursor >= visible) scroll = s_pkt_cursor - visible + 1;

    for (int i = 0; i < visible; i++) {
        int list_idx = scroll + i;
        if (list_idx >= s_disp_count) break;
        int idx = ((s_disp_head - 1 - list_idx) % DISP_MAX + DISP_MAX) % DISP_MAX;
        const disp_frame_t* d = &s_disp[idx];
        bool hl = (list_idx == s_pkt_cursor);

        int y = list_y + i * lh2;
        if (hl) bs_gfx_fill_rect(0, y, bs_gfx_width(), lh2, g_bs_theme.dim);

        if (hl) {
            /* Highlighted: label + RSSI + len + all 3 full MACs (carousel if too wide) */
            if (d->has_addr && d->has_addr3) {
                snprintf(buf, sizeof buf,
                         "%-8s %d %dB"
                         "  s:%02x:%02x:%02x:%02x:%02x:%02x"
                         "  d:%02x:%02x:%02x:%02x:%02x:%02x"
                         "  b:%02x:%02x:%02x:%02x:%02x:%02x",
                         d->label, (int)d->rssi, (int)d->len,
                         d->addr2[0],d->addr2[1],d->addr2[2],
                         d->addr2[3],d->addr2[4],d->addr2[5],
                         d->addr1[0],d->addr1[1],d->addr1[2],
                         d->addr1[3],d->addr1[4],d->addr1[5],
                         d->addr3[0],d->addr3[1],d->addr3[2],
                         d->addr3[3],d->addr3[4],d->addr3[5]);
            } else if (d->has_addr) {
                snprintf(buf, sizeof buf,
                         "%-8s %d %dB"
                         "  s:%02x:%02x:%02x:%02x:%02x:%02x"
                         "  d:%02x:%02x:%02x:%02x:%02x:%02x",
                         d->label, (int)d->rssi, (int)d->len,
                         d->addr2[0],d->addr2[1],d->addr2[2],
                         d->addr2[3],d->addr2[4],d->addr2[5],
                         d->addr1[0],d->addr1[1],d->addr1[2],
                         d->addr1[3],d->addr1[4],d->addr1[5]);
            } else {
                snprintf(buf, sizeof buf, "%-8s %d %dB",
                         d->label, (int)d->rssi, (int)d->len);
            }
            int avail  = bs_gfx_width() - 16;
            int full_w = bs_gfx_text_w(buf, ts2);
            if (full_w <= avail) {
                bs_gfx_text(8, y, buf, g_bs_theme.accent, ts2);
            } else {
                int flen  = (int)strlen(buf);
                int max_c = (int)((long)flen * avail / full_w);
                if (max_c < 1) max_c = 1;
                if (max_c >= (int)sizeof(buf)) max_c = (int)sizeof(buf) - 1;
                int off = s_carousel_offset % flen;
                char win[80];
                for (int k = 0; k < max_c; k++)
                    win[k] = buf[(off + k) % flen];
                win[max_c] = '\0';
                bs_gfx_text(8, y, win, g_bs_theme.accent, ts2);
            }
        } else {
            /* Compact: label + abbreviated MACs (3 bytes each, no colons) */
            if (d->has_addr && d->has_addr3) {
                snprintf(buf, sizeof buf, "%-9s %02x%02x%02x %02x%02x%02x %02x%02x%02x",
                         d->label,
                         d->addr2[0],d->addr2[1],d->addr2[2],
                         d->addr1[0],d->addr1[1],d->addr1[2],
                         d->addr3[0],d->addr3[1],d->addr3[2]);
            } else if (d->has_addr) {
                snprintf(buf, sizeof buf, "%-9s %02x%02x%02x %02x%02x%02x",
                         d->label,
                         d->addr2[0],d->addr2[1],d->addr2[2],
                         d->addr1[0],d->addr1[1],d->addr1[2]);
            } else {
                snprintf(buf, sizeof buf, "%-9s %4d", d->label, (int)d->rssi);
            }
            bs_gfx_text(8, y, buf, g_bs_theme.primary, ts2);
        }
    }

    if (s_disp_count == 0)
        bs_gfx_text(8, list_y, "waiting for frames...", g_bs_theme.dim, ts);

    bs_ui_draw_hint("<<>>:filter  UP/DN:select  SEL=expand  BACK=stop");
    bs_gfx_present();
}

/* ── Menu item helpers ───────────────────────────────────────────────────── */

static int menu_item_count(void) {
    return (s_chmode == SNF_CH_FIXED) ? 5 : 4;
}

static int cursor_to_item(int cursor) {
    if (s_chmode == SNF_CH_AUTO && cursor >= 3) return cursor + 1;
    return cursor;
}

/* ── Public entry point ──────────────────────────────────────────────────── */

void wifi_sniffer_run(const bs_arch_t* arch) {
    s_state          = SNF_MENU;
    s_filter         = SNF_FILTER_ALL;
    s_chmode         = SNF_CH_AUTO;
    s_channel        = 1;
    s_menu_cursor    = 0;
    s_dirty          = true;
    s_pcap           = NULL;
    s_pkt_cursor     = 0;
    s_pkt_expanded   = false;
    s_mac_filter_mode = MAC_FILTER_NONE;
    memset(s_mac_filter_addr, 0, 6);
    s_carousel_offset = 0;
    s_carousel_ms     = 0;
    disp_reset();

    for (;;) {
        uint32_t now = arch->millis();
        s_cb_millis  = now;

        /* ── Input ── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            if (s_state == SNF_MENU) {
                int n = menu_item_count();
                switch (nav) {
                    case BS_NAV_UP:   case BS_NAV_PREV:
                        s_menu_cursor = (s_menu_cursor + n - 1) % n;
                        s_dirty = true; break;
                    case BS_NAV_DOWN: case BS_NAV_NEXT:
                        s_menu_cursor = (s_menu_cursor + 1) % n;
                        s_dirty = true; break;
                    case BS_NAV_SELECT: {
                        int item = cursor_to_item(s_menu_cursor);
                        if (item == 0) {
                            start_sniff(arch);
                        } else if (item == 1) {
                            s_filter = (snf_filter_t)((s_filter + 1) % SNF_FILTER_COUNT);
                            s_dirty = true;
                        } else if (item == 2) {
                            s_chmode = (s_chmode == SNF_CH_AUTO)
                                     ? SNF_CH_FIXED : SNF_CH_AUTO;
                            s_dirty = true;
                        } else if (item == 3) {
                            s_channel = (s_channel % 13) + 1;
                            s_dirty = true;
                        } else {
                            return;
                        }
                        break;
                    }
                    case BS_NAV_LEFT:
                    case BS_NAV_RIGHT: {
                        int item = cursor_to_item(s_menu_cursor);
                        int dir  = (nav == BS_NAV_LEFT) ? -1 : 1;
                        if (item == 1) {
                            s_filter = (snf_filter_t)
                                ((s_filter + SNF_FILTER_COUNT + dir) % SNF_FILTER_COUNT);
                            s_dirty = true;
                        } else if (item == 3) {
                            if (dir > 0)
                                s_channel = (s_channel % 13) + 1;
                            else
                                s_channel = (s_channel == 1) ? 13 : s_channel - 1;
                            s_dirty = true;
                        }
                        break;
                    }
                    case BS_NAV_BACK: return;
                    default: break;
                }
            } else { /* SNF_RUNNING */
                if (s_pkt_expanded) {
                    /* Expanded view navigation */
                    switch (nav) {
                        case BS_NAV_SELECT:
                        case BS_NAV_BACK:
                            s_pkt_expanded = false;
                            s_dirty = true;
                            break;
                        case BS_NAV_LEFT:
                        case BS_NAV_RIGHT: {
                            /* Cycle MAC filter using current packet's addresses */
                            int dir = (nav == BS_NAV_LEFT) ? -1 : 1;
                            mac_filter_mode_t next =
                                (mac_filter_mode_t)((s_mac_filter_mode + MAC_FILTER_COUNT + dir)
                                                    % MAC_FILTER_COUNT);
                            const disp_frame_t* d = disp_at_cursor();
                            if (next == MAC_FILTER_NONE) {
                                s_mac_filter_mode = MAC_FILTER_NONE;
                            } else if (d) {
                                s_mac_filter_mode = next;
                                switch (next) {
                                    case MAC_FILTER_ADDR2:
                                        if (d->has_addr)  memcpy(s_mac_filter_addr, d->addr2, 6);
                                        break;
                                    case MAC_FILTER_ADDR1:
                                        if (d->has_addr)  memcpy(s_mac_filter_addr, d->addr1, 6);
                                        break;
                                    case MAC_FILTER_ADDR3:
                                        if (d->has_addr3) memcpy(s_mac_filter_addr, d->addr3, 6);
                                        else s_mac_filter_mode = MAC_FILTER_NONE;
                                        break;
                                    default: break;
                                }
                            }
                            s_dirty = true;
                            break;
                        }
                        default: break;
                    }
                } else {
                    /* Normal running navigation */
                    switch (nav) {
                        case BS_NAV_BACK:
                            stop_sniff();
                            s_state = SNF_MENU;
                            s_dirty = true;
                            break;
                        /* UP = toward top of list = newer (cursor 0 = newest)  */
                        case BS_NAV_UP: case BS_NAV_PREV:
                            if (s_pkt_cursor > 0) {
                                s_pkt_cursor--;
                                s_carousel_offset = 0;
                                s_carousel_ms     = now;
                                s_dirty = true;
                            }
                            break;
                        /* DOWN = toward bottom of list = older                  */
                        case BS_NAV_DOWN: case BS_NAV_NEXT:
                            if (s_pkt_cursor < s_disp_count - 1) {
                                s_pkt_cursor++;
                                s_carousel_offset = 0;
                                s_carousel_ms     = now;
                                s_dirty = true;
                            }
                            break;
                        case BS_NAV_LEFT:
                            s_filter = (snf_filter_t)
                                ((s_filter + SNF_FILTER_COUNT - 1) % SNF_FILTER_COUNT);
                            s_carousel_offset = 0;
                            s_dirty = true; break;
                        case BS_NAV_RIGHT:
                            s_filter = (snf_filter_t)
                                ((s_filter + 1) % SNF_FILTER_COUNT);
                            s_carousel_offset = 0;
                            s_dirty = true; break;
                        case BS_NAV_SELECT:
                            if (s_disp_count > 0) {
                                s_pkt_pinned   = *disp_at_cursor(); /* snapshot */
                                s_pkt_expanded = true;
                                s_dirty = true;
                            }
                            break;
                        default: break;
                    }
                }
            }
        }

        /* ── Running: drain ring buffer ── */
        if (s_state == SNF_RUNNING) {
            hop_channel(now);

            ring_slot_t slot;
            bool got_frame = false;
            while (ring_pop(&slot)) {
                if (!passes_filter(slot.data, slot.len)) continue;
                s_total_bytes += slot.len;
                wifi_pps_tick(&s_pps, now);
                disp_push(&slot);
                got_frame = true;

                if (s_pcap) {
                    uint32_t ts_sec  = slot.ts_ms / 1000;
                    uint32_t ts_usec = (slot.ts_ms % 1000) * 1000;
                    bs_pcap_write(s_pcap, slot.data, slot.len,
                                  ts_sec, ts_usec);
                }
            }
            if (got_frame) s_dirty = true;

            /* Advance carousel for highlighted row */
            if (!s_pkt_expanded && (now - s_carousel_ms) >= CAROUSEL_INTERVAL_MS) {
                s_carousel_ms = now;
                s_carousel_offset++;
                s_dirty = true;
            }
        }

        /* ── Draw ── */
        if (s_dirty) {
            s_dirty = false;
            if (s_state == SNF_MENU)
                draw_menu();
            else
                draw_running();
        }

        arch->delay_ms(1);
    }
}

#endif /* BS_HAS_WIFI */
