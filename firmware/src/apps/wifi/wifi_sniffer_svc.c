/*
 * wifi_sniffer_svc.c - WiFi sniffer service (ring buffer + monitor, no UI).
 */
#include "bs/bs_wifi.h"
#ifdef BS_HAS_WIFI

#include "wifi_sniffer_svc.h"
#include "bs/bs_arch.h"
#include "bs/bs_wifi.h"

#include <string.h>
#include <stdatomic.h>

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

#define RING_SLOTS     32
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

static bool ring_push(const uint8_t* frame, uint16_t len, int8_t rssi,
                      uint32_t ts_ms) {
    if (len > RING_FRAME_MAX) { return false; }
    unsigned head = atomic_load_explicit(&s_ring_head, memory_order_relaxed);
    unsigned next = (head + 1) % RING_SLOTS;
    unsigned tail = atomic_load_explicit(&s_ring_tail, memory_order_acquire);
    if (next == tail) return false;
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
    atomic_store_explicit(&s_ring_tail, (tail + 1) % RING_SLOTS,
                          memory_order_release);
    return true;
}

/* ── Internal state ──────────────────────────────────────────────────────── */

static const bs_arch_t*  s_arch;
static sniffer_svc_state_t s_state;
static uint8_t           s_channel;
static bool              s_auto_hop;
static uint32_t          s_hop_ms;
static uint32_t          s_last_hop_ms;
static sniffer_svc_pkt_fn s_cb;
static void*             s_cb_ctx;
static uint32_t          s_count;
static uint32_t          s_dropped;

/* ── Promiscuous callback (WiFi task / ISR context) ──────────────────────── */

static void promisc_cb(const uint8_t* frame, uint16_t len,
                       int8_t rssi, void* ctx) {
    (void)ctx;
    uint32_t ts = s_arch ? s_arch->millis() : 0;
    if (!ring_push(frame, len, rssi, ts))
        s_dropped++;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

void sniffer_svc_init(const bs_arch_t* arch) {
    s_arch  = arch;
    s_state = SNIFFER_SVC_IDLE;
}

void sniffer_svc_start(uint8_t channel, uint32_t hop_ms,
                       sniffer_svc_pkt_fn cb, void* ctx) {
    s_cb       = cb;
    s_cb_ctx   = ctx;
    s_auto_hop = (channel == 0);
    s_hop_ms   = (hop_ms > 0) ? hop_ms : 500;
    s_channel  = s_auto_hop ? 1 : channel;
    s_count    = 0;
    s_dropped  = 0;
    s_last_hop_ms = 0;

    atomic_store(&s_ring_head, 0);
    atomic_store(&s_ring_tail, 0);

    bs_wifi_monitor_start(s_channel, promisc_cb, NULL);
    s_state = SNIFFER_SVC_RUNNING;
}

void sniffer_svc_stop(void) {
    if (s_state == SNIFFER_SVC_RUNNING)
        bs_wifi_monitor_stop();
    s_state = SNIFFER_SVC_IDLE;
}

void sniffer_svc_set_channel(uint8_t ch) {
    if (s_state != SNIFFER_SVC_RUNNING) return;
    s_channel  = ch;
    s_auto_hop = false;
    bs_wifi_set_channel(ch);
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

void sniffer_svc_tick(uint32_t now_ms) {
    if (s_state != SNIFFER_SVC_RUNNING) return;

    /* Auto-hop */
    if (s_auto_hop && s_last_hop_ms == 0) s_last_hop_ms = now_ms;
    if (s_auto_hop && (now_ms - s_last_hop_ms) >= s_hop_ms) {
        s_channel = (uint8_t)(s_channel % 13) + 1;
        bs_wifi_set_channel(s_channel);
        s_last_hop_ms = now_ms;
    }

    /* Drain ring */
    ring_slot_t slot;
    while (ring_pop(&slot)) {
        s_count++;
        if (s_cb) s_cb(slot.data, slot.len, slot.rssi, slot.ts_ms, s_cb_ctx);
    }
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

sniffer_svc_state_t sniffer_svc_state(void)   { return s_state;   }
uint8_t             sniffer_svc_channel(void)  { return s_channel; }
uint32_t            sniffer_svc_count(void)    { return s_count;   }
uint32_t            sniffer_svc_dropped(void)  { return s_dropped; }

#endif /* BS_HAS_WIFI */
