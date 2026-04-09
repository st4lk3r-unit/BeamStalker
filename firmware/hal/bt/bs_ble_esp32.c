/*
 * bs_ble_esp32.c - BLE backend for ESP32/S3 (Arduino + ESP-IDF Bluedroid).
 *
 * Capabilities advertised:
 *   BS_BLE_CAP_ADVERTISE  — raw AD payload advertising via esp_ble_gap_*
 *   BS_BLE_CAP_SCAN       — passive advertisement scanning
 *   BS_BLE_CAP_RAND_ADDR  — random address via esp_ble_gap_set_rand_addr()
 *
 * Raw payload design:
 *   bs_ble_adv_start() passes the caller's AD bytes directly to
 *   esp_ble_gap_config_adv_data_raw() with no modification.  This allows
 *   spoofing any vendor-specific BLE device (Apple, Samsung, Google, etc.)
 *   by supplying the correct byte sequence.
 *
 * Coexistence with WiFi:
 *   ESP32 BLE and WiFi share the 2.4 GHz radio via time-division.  Both can
 *   be active simultaneously but throughput / latency of each is reduced.
 *   Call bs_ble_init() after bs_wifi_init() — the coexistence controller
 *   is managed by the IDF and requires no extra action.
 *
 * Note on tx power:
 *   esp_ble_tx_power_set() accepts esp_power_level_t enum values.
 *   We convert the dBm request to the nearest supported level.
 */
#ifdef BS_BLE_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "bs/bs_ble.h"
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"      /* esp_bluedroid_init/enable, esp_bt_controller_* */
#include "esp_gap_ble_api.h"  /* esp_ble_gap_*, esp_power_level_t              */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ── Private state ──────────────────────────────────────────────────────── */

static bool                s_init               = false;
static bool                s_advertising        = false;
static volatile bool       s_scanning           = false;
static volatile bool       s_scan_start_pending = false;
static bs_ble_scan_cb_t    s_scan_cb            = NULL;
static void*               s_scan_ctx           = NULL;
static volatile bool       s_addr_set           = false;  /* set by GAP addr-set event */

/* ── dBm → esp_power_level_t ─────────────────────────────────────────────── */

static esp_power_level_t dbm_to_esp_level(int dbm) {
    /* ESP32 levels: -12,-9,-6,-3,0,3,6,9 dBm → indices 0..7 */
    if (dbm <= -12) return ESP_PWR_LVL_N12;
    if (dbm <=  -9) return ESP_PWR_LVL_N9;
    if (dbm <=  -6) return ESP_PWR_LVL_N6;
    if (dbm <=  -3) return ESP_PWR_LVL_N3;
    if (dbm <=   0) return ESP_PWR_LVL_N0;
    if (dbm <=   3) return ESP_PWR_LVL_P3;
    if (dbm <=   6) return ESP_PWR_LVL_P6;
    return ESP_PWR_LVL_P9;
}

/* ── GAP event handler ───────────────────────────────────────────────────── */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            if (!s_scan_cb) break;
            if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
            bs_ble_scan_result_t r = (bs_ble_scan_result_t){0};
            memcpy(r.addr, param->scan_rst.bda, 6);
            r.rssi = (int8_t)param->scan_rst.rssi;
            uint8_t adv_len = param->scan_rst.adv_data_len;
            if (adv_len > 31) adv_len = 31;
            memcpy(r.adv.data, param->scan_rst.ble_adv, adv_len);
            r.adv.len = adv_len;
            s_scan_cb(&r, s_scan_ctx);
            break;
        }
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (!s_scan_start_pending) break;
            if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                s_scan_start_pending = false;
                s_scan_cb  = NULL;
                s_scan_ctx = NULL;
                break;
            }
            if (esp_ble_gap_start_scanning(0) != ESP_OK) {
                s_scan_start_pending = false;
                s_scan_cb  = NULL;
                s_scan_ctx = NULL;
            }
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                s_scanning = true;
            } else {
                s_scan_cb  = NULL;
                s_scan_ctx = NULL;
            }
            s_scan_start_pending = false;
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            s_scanning           = false;
            s_scan_start_pending = false;
            s_scan_cb            = NULL;
            s_scan_ctx           = NULL;
            break;

        case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
            s_addr_set = true;
            break;
        default:
            break;
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

int bs_ble_init(const bs_arch_t* arch) {
    (void)arch;
    if (s_init) return 0;

    /* The BT controller may already be initialised (but disabled) from a
     * previous bs_ble_deinit() call.  We intentionally skip deinit there to
     * preserve the coexistence framework — see bs_ble_deinit() comment.     */
    esp_bt_controller_status_t ctrl = esp_bt_controller_get_status();
    if (ctrl == ESP_BT_CONTROLLER_STATUS_IDLE) {
        /* Release Classic BT memory — BeamStalker uses BLE-only.
         * This frees ~20-30 KB before the controller allocates its BLE pools,
         * which is critical on no-PSRAM targets (Cardputer) where WiFi + BLE
         * would otherwise exhaust the heap and trigger an internal IDF abort. */
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK) return -2;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) return -2;
    }
    if (esp_bluedroid_init()  != ESP_OK) return -2;
    if (esp_bluedroid_enable() != ESP_OK) return -2;
    if (esp_ble_gap_register_callback(gap_event_handler) != ESP_OK) return -2;

    s_scanning           = false;
    s_scan_start_pending = false;
    s_scan_cb            = NULL;
    s_scan_ctx           = NULL;
    s_init               = true;
    return 0;
}

void bs_ble_deinit(void) {
    if (!s_init) return;
    bs_ble_adv_stop();
    bs_ble_scan_stop();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    /* Intentionally NOT calling esp_bt_controller_deinit():
     * on ESP-IDF 4.4 that tears down the shared coexistence framework,
     * making subsequent esp_wifi_init() fail.  Leaving the controller in
     * INITED (disabled) state is safe — it holds no TX/RX buffers and the
     * radio is off; WiFi can still initialise and use the coex scheduler.  */
    s_scanning           = false;
    s_scan_start_pending = false;
    s_scan_cb            = NULL;
    s_scan_ctx           = NULL;
    s_init               = false;
}

uint32_t bs_ble_caps(void) {
    return BS_BLE_CAP_ADVERTISE | BS_BLE_CAP_SCAN | BS_BLE_CAP_RAND_ADDR;
}

/* ── Address ─────────────────────────────────────────────────────────────── */

int bs_ble_set_addr(const uint8_t addr[6]) {
    if (!s_init) return -2;
    uint8_t tmp[6];
    memcpy(tmp, addr, 6);
    /* Fire-and-forget: the BLE HCI command queue is FIFO, so set_rand_addr
     * is guaranteed to execute before any subsequent start_advertising call.
     * No explicit wait needed — vTaskDelay blocking here kills spam throughput. */
    esp_err_t err = esp_ble_gap_set_rand_addr(tmp);
    return (err == ESP_OK) ? 0 : -2;
}

/* ── TX power ────────────────────────────────────────────────────────────── */

int bs_ble_set_tx_power(int dbm) {
    if (!s_init) return -2;
    esp_power_level_t lvl = dbm_to_esp_level(dbm);
    /* Set power for both advertising and scanning */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,  lvl);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, lvl);
    return 0;
}

/* ── Advertising ─────────────────────────────────────────────────────────── */

int bs_ble_adv_start(const bs_ble_adv_data_t* data,
                                 uint32_t interval_ms) {
    if (!s_init || !data)         return -2;
    if (s_advertising) bs_ble_adv_stop();

    /* Raw payload — delivered byte-for-byte, no stack modification */
    esp_err_t err = esp_ble_gap_config_adv_data_raw(
                        (uint8_t*)data->data, data->len);
    if (err != ESP_OK) return -2;

    /* Convert ms to BLE units (0.625 ms per unit, min=0x20, max=0x4000) */
    uint16_t units = (uint16_t)(interval_ms * 8 / 5);
    if (units < 0x0020) units = 0x0020;
    if (units > 0x4000) units = 0x4000;

    esp_ble_adv_params_t params = (esp_ble_adv_params_t){0};
    params.adv_int_min       = units;
    params.adv_int_max       = units;
    params.adv_type          = ADV_TYPE_IND;
    params.own_addr_type     = BLE_ADDR_TYPE_RANDOM;
    params.channel_map       = ADV_CHNL_ALL;
    params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    err = esp_ble_gap_start_advertising(&params);
    if (err != ESP_OK) return -2;

    s_advertising = true;
    return 0;
}

void bs_ble_adv_stop(void) {
    if (!s_init || !s_advertising) return;
    esp_ble_gap_stop_advertising();
    s_advertising = false;
}

int bs_ble_adv_update(const bs_ble_adv_data_t* data) {
    if (!s_init || !data) return -2;
    /* Update payload in-place; advertising continues without a gap */
    esp_err_t err = esp_ble_gap_config_adv_data_raw(
                        (uint8_t*)data->data, data->len);
    return (err == ESP_OK) ? 0 : -2;
}

/* ── Scanning ────────────────────────────────────────────────────────────── */

int bs_ble_scan_start(bs_ble_scan_cb_t cb, void* ctx) {
    if (!s_init || !cb) return -2;
    if (s_scanning || s_scan_start_pending) return 0;

    s_scan_cb            = cb;
    s_scan_ctx           = ctx;
    s_scan_start_pending = true;

    esp_ble_scan_params_t scan_params = (esp_ble_scan_params_t){0};
    scan_params.scan_type          = BLE_SCAN_TYPE_PASSIVE;
    scan_params.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;  /* no random addr needed for rx */
    scan_params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scan_params.scan_interval      = 0x0050;   /* 50 ms */
    scan_params.scan_window        = 0x0030;   /* 30 ms */
    scan_params.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE;

    esp_err_t err = esp_ble_gap_set_scan_params(&scan_params);
    if (err != ESP_OK) {
        s_scan_start_pending = false;
        s_scan_cb  = NULL;
        s_scan_ctx = NULL;
        return -2;
    }

    /* Start scanning from ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT. */
    return 0;
}

void bs_ble_scan_stop(void) {
    if (!s_init) return;

    if (s_scan_start_pending) {
        s_scan_start_pending = false;
        s_scan_cb  = NULL;
        s_scan_ctx = NULL;
        return;
    }
    if (!s_scanning) return;

    esp_ble_gap_stop_scanning();
}

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_BLE_ESP32 */
