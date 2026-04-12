/*
 * bs_wifi_esp32.c - WiFi backend for ESP32/S3 (pure IDF, no Arduino WiFi.h).
 *
 * Capabilities:
 *   BS_WIFI_CAP_CONNECT  — STA association
 *   BS_WIFI_CAP_SCAN     — async AP scan
 *   BS_WIFI_CAP_SNIFF    — promiscuous 802.11 rx
 *   BS_WIFI_CAP_INJECT   — raw frame tx via esp_wifi_80211_tx()
 *
 * Pure IDF rationale: Arduino WiFi.h calls esp_wifi_init() with nvs_enable=1,
 * which malloc()s an NVS config buffer from SRAM.  After SIC+SD fragment the
 * heap that alloc fails with "wifi nvs cfg alloc out of memory".  We call
 * esp_wifi_init() directly with nvs_enable=0 to avoid it.
 *
 * esp_prom_cb() fires from the WiFi task — keep it short, no bs_wifi_* calls.
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "bs/bs_wifi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_idf_version.h"

/* ── Private state ──────────────────────────────────────────────────────── */

static bs_wifi_state_t    s_state     = BS_WIFI_STATE_OFF;
static uint32_t           s_caps      = 0;
static bs_wifi_frame_cb_t s_frame_cb  = NULL;
static void*              s_frame_ctx = NULL;
static esp_netif_t*       s_sta_netif = NULL;
static esp_netif_t*       s_ap_netif  = NULL;
static volatile bool      s_scan_done = false;
static volatile bool      s_connected = false;
static volatile bool      s_ap_active = false;

/* ── Auth conversion ─────────────────────────────────────────────────────── */

static bs_wifi_auth_t esp_to_bs_auth(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return BS_WIFI_AUTH_OPEN;
        case WIFI_AUTH_WEP:             return BS_WIFI_AUTH_WEP;
        case WIFI_AUTH_WPA_PSK:         return BS_WIFI_AUTH_WPA_PSK;
        case WIFI_AUTH_WPA2_PSK:        return BS_WIFI_AUTH_WPA2_PSK;
        case WIFI_AUTH_WPA_WPA2_PSK:    return BS_WIFI_AUTH_WPA_WPA2_PSK;
        case WIFI_AUTH_WPA3_PSK:        return BS_WIFI_AUTH_WPA3_PSK;
        default:                        return BS_WIFI_AUTH_UNKNOWN;
    }
}

/* ── WiFi event handler ──────────────────────────────────────────────────── */

static void wifi_ev(void* arg, esp_event_base_t base,
                    int32_t id, void* data) {
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            s_scan_done = true;
            if (s_state == BS_WIFI_STATE_SCANNING)
                s_state = BS_WIFI_STATE_IDLE;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_connected = true;
            s_state = BS_WIFI_STATE_CONNECTED;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_connected = false;
            if (s_state == BS_WIFI_STATE_CONNECTING ||
                s_state == BS_WIFI_STATE_CONNECTED)
                s_state = BS_WIFI_STATE_IDLE;
            break;
        default: break;
    }
}

/* ── Promiscuous callback ────────────────────────────────────────────────── */

static void esp_prom_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_frame_cb || type == WIFI_PKT_MISC) return;
    const wifi_promiscuous_pkt_t* pkt =
        (const wifi_promiscuous_pkt_t*)buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    if (len >= 4) len -= 4;     /* strip 4-byte FCS appended by hardware */
    s_frame_cb(pkt->payload, len, (int8_t)pkt->rx_ctrl.rssi,
               s_frame_ctx);
}

/* ── Shared promiscuous entry (used by sniff and monitor) ───────────────── */

static int enter_promiscuous(uint8_t channel,
                              bs_wifi_frame_cb_t cb, void* ctx,
                              uint32_t filter_mask) {
    esp_wifi_disconnect();       /* drop any active association */
    s_connected = false;

    s_frame_cb  = cb;
    s_frame_ctx = ctx;

    wifi_promiscuous_filter_t f = (wifi_promiscuous_filter_t){0};
    f.filter_mask = filter_mask;
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(esp_prom_cb);

    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) return -2;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_state = BS_WIFI_STATE_MONITOR;
    return 0;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bs_wifi_init(const bs_arch_t* arch) {
    (void)arch;
    if (s_state != BS_WIFI_STATE_OFF) return 0;

    /* esp_netif_init / esp_event_loop_create_default are idempotent;
     * ESP_ERR_INVALID_STATE means the framework already called them.        */
    esp_netif_init();
    esp_event_loop_create_default();

    /* Reuse existing STA netif if present — recreating it would assert-abort. */
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif)
        s_sta_netif = esp_netif_create_default_wifi_sta();

    /* On ESP-IDF 4.4, esp_wifi_deinit() leaves allocator state that makes a
     * subsequent esp_wifi_init() fail.  bs_wifi_deinit() therefore keeps the
     * driver allocated (stop-only).  Detect that case: if esp_wifi_get_mode()
     * succeeds the driver is already allocated and we skip esp_wifi_init().  */
    /* bs_wifi_deinit() keeps the driver allocated (stop-only) to work around
     * an ESP-IDF 4.4 bug where esp_wifi_deinit() + esp_wifi_init() fails on
     * re-init.  Only call esp_wifi_init() when the driver is not yet alive.  */
    wifi_mode_t _mode;
    if (esp_wifi_get_mode(&_mode) == ESP_ERR_WIFI_NOT_INIT) {
        /* Dynamic TX buffers: static buffers need ~12.8 KB contiguous SRAM at
         * init time; after SIC+SD fragment the heap that alloc fails.  Dynamic
         * buffers allocate on demand and succeed on a fragmented heap.
         * 32 TX buffers: 16 was too shallow — a single raw-inject burst filled
         * it, causing subsequent esp_wifi_80211_tx() calls to fail silently.  */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.nvs_enable          = 0;
        cfg.static_tx_buf_num   = 0;
        cfg.dynamic_tx_buf_num  = 32;
        cfg.tx_buf_type         = 1;    /* 0=static, 1=dynamic               */
        cfg.cache_tx_buf_num    = 4;    /* must be non-zero when tx_buf_type=1 */
        cfg.static_rx_buf_num   = 2;    /* minimum valid value (1 is rejected) */
        cfg.dynamic_rx_buf_num  = 16;

        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            printf("[wifi] esp_wifi_init failed: 0x%x (%s)\n",
                   (unsigned)err, esp_err_to_name(err));
            return -2;
        }
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    /* 84 = 21 dBm (units of 0.25 dBm, range 8-84); driver clamps to board max */
    esp_wifi_set_max_tx_power(84);

    s_caps  = BS_WIFI_CAP_CONNECT | BS_WIFI_CAP_SCAN
            | BS_WIFI_CAP_SNIFF   | BS_WIFI_CAP_INJECT | BS_WIFI_CAP_AP;
    s_state = BS_WIFI_STATE_IDLE;
    return 0;
}

void bs_wifi_deinit(void) {
    if (s_state == BS_WIFI_STATE_OFF) return;
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev);
    esp_wifi_set_promiscuous(false);
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* Intentionally NOT calling esp_wifi_deinit() — on ESP-IDF 4.4 a full
     * deinit leaves internal allocator state that prevents a successful
     * re-init.  Keeping the driver allocated but stopped (~30 KB) allows
     * bs_wifi_init() to skip esp_wifi_init() and just call esp_wifi_start(). */
    s_state     = BS_WIFI_STATE_OFF;
    s_caps      = 0;
    s_frame_cb  = NULL;
    s_frame_ctx = NULL;
    s_connected = false;
    s_ap_active = false;
}

uint32_t bs_wifi_caps(void) { return s_caps; }

bs_wifi_state_t bs_wifi_state(void) {
    return s_state;
}

/* ── Scan ────────────────────────────────────────────────────────────────── */

int bs_wifi_scan_start(void) {
    if (!(s_caps & BS_WIFI_CAP_SCAN))     return -1;
    if (s_state == BS_WIFI_STATE_MONITOR) return -2;

    /* "CN" + MANUAL: 13 channels; MANUAL prevents the STA from reverting to
     * the AP's advertised country after association (which would drop ch12/13). */
    wifi_country_t ctry = (wifi_country_t){0};
    ctry.cc[0]  = 'C'; ctry.cc[1] = 'N';
    ctry.schan  = 1;
    ctry.nchan  = 13;
    ctry.policy = WIFI_COUNTRY_POLICY_MANUAL;
    esp_wifi_set_country(&ctry);

    wifi_scan_config_t sc = (wifi_scan_config_t){0};
    sc.show_hidden = true;
    /* Passive scan: listen for beacons; 200 ms covers the 100 ms beacon interval. */
    sc.scan_type              = WIFI_SCAN_TYPE_PASSIVE;
    sc.scan_time.passive      = 200;

    s_scan_done = false;
    esp_err_t err = esp_wifi_scan_start(&sc, /*block=*/false);
    if (err != ESP_OK) return -3;

    s_state = BS_WIFI_STATE_SCANNING;
    return 0;
}

bool bs_wifi_scan_done(void) {
    if (s_state != BS_WIFI_STATE_SCANNING) return true;
    return s_scan_done;
}

int bs_wifi_scan_results(bs_wifi_ap_t* out, int max_count) {
    if (!out || max_count <= 0) return -1;
    if (!s_scan_done) return 0;

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) return 0;

    uint16_t count = (ap_num < (uint16_t)max_count) ? ap_num : (uint16_t)max_count;
    wifi_ap_record_t* recs =
        (wifi_ap_record_t*)malloc(count * sizeof(wifi_ap_record_t));
    if (!recs) return -2;

    esp_err_t err = esp_wifi_scan_get_ap_records(&count, recs);
    if (err != ESP_OK) { free(recs); return -3; }

    for (int i = 0; i < (int)count; i++) {
        strncpy(out[i].ssid, (const char*)recs[i].ssid, 32);
        out[i].ssid[32] = '\0';
        memcpy(out[i].bssid, recs[i].bssid, 6);
        out[i].rssi    = recs[i].rssi;
        out[i].channel = recs[i].primary;
        out[i].auth    = esp_to_bs_auth(recs[i].authmode);
    }
    free(recs);
    return (int)count;
}

/* ── Connect ─────────────────────────────────────────────────────────────── */

int bs_wifi_connect(const char* ssid, const char* password) {
    if (!(s_caps & BS_WIFI_CAP_CONNECT))  return -1;
    if (s_state == BS_WIFI_STATE_MONITOR) return -2;

    wifi_config_t wcfg = (wifi_config_t){0};
    strncpy((char*)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    if (password)
        strncpy((char*)wcfg.sta.password, password,
                sizeof(wcfg.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) return -3;

    s_state = BS_WIFI_STATE_CONNECTING;
    return 0;
}

void bs_wifi_disconnect(void) {
    esp_wifi_disconnect();
    s_connected = false;
    s_state = BS_WIFI_STATE_IDLE;
}

int bs_wifi_get_ip(char* buf, size_t len) {
    if (!s_connected || !s_sta_netif) return -1;
    esp_netif_ip_info_t info = (esp_netif_ip_info_t){0};
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) return -2;
    if (info.ip.addr == 0) return -1;
    esp_ip4addr_ntoa(&info.ip, buf, (int)len);
    return 0;
}

/* ── SoftAP / APSTA hosting ────────────────────────────────────────────── */

int bs_wifi_ap_start(const char* ssid, uint8_t channel, const char* password) {
    if (s_state == BS_WIFI_STATE_OFF && bs_wifi_init(NULL) != 0) return -2;
    if (!(s_caps & BS_WIFI_CAP_AP)) return -1;

    if (!ssid || ssid[0] == '\0') ssid = "FreeWifi";

    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif)
        s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return -2;

    esp_wifi_set_promiscuous(false);
    esp_wifi_disconnect();

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return -2;

    wifi_config_t ap_cfg = (wifi_config_t){0};
    size_t ssid_len = strnlen(ssid, 32);
    memcpy(ap_cfg.ap.ssid, ssid, ssid_len);
    ap_cfg.ap.ssid_len        = (uint8_t)ssid_len;
    ap_cfg.ap.channel         = channel ? channel : 1;
    ap_cfg.ap.max_connection  = 8;
    ap_cfg.ap.beacon_interval = 100;

    size_t pass_len = password ? strnlen(password, 64) : 0;
    if (pass_len >= 8) {
        memcpy(ap_cfg.ap.password, password, pass_len);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) return -2;
    s_ap_active = true;
    if (s_state == BS_WIFI_STATE_OFF) s_state = BS_WIFI_STATE_IDLE;
    return 0;
}

void bs_wifi_ap_stop(void) {
    if (s_state == BS_WIFI_STATE_OFF) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_active = false;
    if (s_state != BS_WIFI_STATE_CONNECTING && s_state != BS_WIFI_STATE_CONNECTED)
        s_state = BS_WIFI_STATE_IDLE;
}

int bs_wifi_ap_client_count(void) {
    if (!(s_caps & BS_WIFI_CAP_AP) || !s_ap_active) return 0;
    wifi_sta_list_t sta = (wifi_sta_list_t){0};
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return -2;
    return (int)sta.num;
}

int bs_wifi_ap_client_list(bs_wifi_sta_t* out, int max_count) {
    if (!(s_caps & BS_WIFI_CAP_AP)) return -1;
    if (!s_ap_active) return 0;
    if (!out || max_count <= 0) return 0;
    wifi_sta_list_t sta = (wifi_sta_list_t){0};
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK) return -2;
    int count = (int)sta.num;
    if (count > max_count) count = max_count;
    for (int i = 0; i < count; i++)
        memcpy(out[i].mac, sta.sta[i].mac, 6);
    return count;
}

int bs_wifi_ap_set_dns_ip(const uint8_t ip[4]) {
    if (!(s_caps & BS_WIFI_CAP_AP) || !ip) return -1;
    if (!s_ap_netif)
        s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) return -2;
    esp_netif_dhcps_stop(s_ap_netif);
    if (esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER,
                               (void*)ip, 4) != ESP_OK) {
        return -2;
    }
    if (esp_netif_dhcps_start(s_ap_netif) != ESP_OK) return -2;
    return 0;
}

int bs_wifi_ap_set_captive_portal_uri(const char* uri) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    if (!(s_caps & BS_WIFI_CAP_AP) || !uri || !uri[0]) return -1;
    if (!s_ap_netif)
        s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) return -2;
    esp_netif_dhcps_stop(s_ap_netif);
    if (esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void*)uri, strlen(uri)) != ESP_OK) {
        return -2;
    }
    if (esp_netif_dhcps_start(s_ap_netif) != ESP_OK) return -2;
    return 0;
#else
    (void)uri;
    return -1;
#endif
}

/* ── Monitor mode ────────────────────────────────────────────────────────── */

int bs_wifi_monitor_start(uint8_t channel,
                                      bs_wifi_frame_cb_t cb, void* ctx) {
    if (!(s_caps & BS_WIFI_CAP_SNIFF)) return -1;
    return enter_promiscuous(channel, cb, ctx, WIFI_PROMIS_FILTER_MASK_ALL);
}

void bs_wifi_monitor_stop(void) {
    esp_wifi_set_promiscuous(false);
    s_frame_cb  = NULL;
    s_frame_ctx = NULL;
    s_state = BS_WIFI_STATE_IDLE;
}

/* ── Sniff (data frames only) ────────────────────────────────────────────── */

int bs_wifi_sniff_start(uint8_t channel,
                                    bs_wifi_frame_cb_t cb, void* ctx) {
    if (!(s_caps & BS_WIFI_CAP_SNIFF)) return -1;
    return enter_promiscuous(channel, cb, ctx, WIFI_PROMIS_FILTER_MASK_DATA);
}

void bs_wifi_sniff_stop(void) {
    bs_wifi_monitor_stop();
}

/* ── Channel ─────────────────────────────────────────────────────────────── */

int bs_wifi_set_channel(uint8_t channel) {
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    return (err == ESP_OK) ? 0 : -2;
}

/* ── TX power ────────────────────────────────────────────────────────────── */

int bs_wifi_set_tx_power(int dbm) {
    /* ESP32 unit is 0.25 dBm; valid range 2–21 dBm (8–84 in IDF units).
     * Driver clamps values outside the board-specific maximum.              */
    int8_t idf_val = (int8_t)(dbm * 4);
    esp_err_t err = esp_wifi_set_max_tx_power(idf_val);
    return (err == ESP_OK) ? 0 : -2;
}

/* ── Raw inject ──────────────────────────────────────────────────────────── */

int bs_wifi_send_raw(bs_wifi_if_t iface,
                                 const uint8_t* frame, uint16_t len) {
    if (!(s_caps & BS_WIFI_CAP_INJECT)) return -1;
    (void)iface; /* STA-only mode; AP interface not started */
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, /*en_sys_seq=*/false);
    return (err == ESP_OK) ? 0 : -2;
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
