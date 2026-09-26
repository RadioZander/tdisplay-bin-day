#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "portal.h"
#include "bin_calendar.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "dns_server.h" // needs esp_netif.h first
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

static const char *TAG = "portal";

#define JOIN_RETRIES    2
#define JOIN_TIMEOUT_MS 30000
#define MAX_NETWORKS    16

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static char s_ssid[20];
static char s_password[12];
static device_config_t s_config; // what will be saved: the network in use and the property
static wifi_ap_record_t s_networks[MAX_NETWORKS];
static int s_network_count;

// A network the page asked to join, until it connects or fails
static bool s_joining;
static TickType_t s_join_started;
static char s_join_ssid[33];
static char s_join_password[65];
static const char *s_join_error = "";

static bool s_saved;
static bool s_finished;
static char s_status[48];

// Decodes a form value in place: "%2B" -> "+", "+" -> " "
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

// Reads `key` from a form-encoded string, decoded. Empty if it's missing.
static void form_value(const char *form, const char *key, char *value, size_t size)
{
    char raw[200];
    if (httpd_query_key_value(form, key, raw, sizeof(raw)) != ESP_OK) {
        raw[0] = '\0';
    }
    url_decode(raw);
    strlcpy(value, raw, size);
}

// Reads a small POST body
static bool read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        return false;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    buf[got] = '\0';
    return true;
}

// Collects a JSON reply and sends it in blocks. Sending lots of small pieces
// is slow, as each one waits on the phone. The web server handles one
// request at a time, so a single writer is enough.
typedef struct {
    httpd_req_t *req;
    size_t len;
    char buf[1024];
} json_writer_t;

static json_writer_t s_json;

static void json_begin(httpd_req_t *req)
{
    s_json.req = req;
    s_json.len = 0;
    httpd_resp_set_type(req, "application/json");
}

static void json_flush(void)
{
    if (s_json.len) {
        httpd_resp_send_chunk(s_json.req, s_json.buf, s_json.len);
        s_json.len = 0;
    }
}

static void json_raw(const char *s)
{
    size_t n = strlen(s);
    if (s_json.len + n > sizeof(s_json.buf)) {
        json_flush();
    }
    if (n > sizeof(s_json.buf)) {
        httpd_resp_send_chunk(s_json.req, s, n);
        return;
    }
    memcpy(s_json.buf + s_json.len, s, n);
    s_json.len += n;
}

// `s` as a JSON string, quotes included
static void json_string(const char *s)
{
    char buf[BIN_ADDRESS_MAX_LEN * 2 + 3];
    size_t n = 0;
    buf[n++] = '"';
    for (; *s && n < sizeof(buf) - 8; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(buf + n, sizeof(buf) - n, "\\u%04x", c);
        } else {
            buf[n++] = c;
        }
    }
    buf[n++] = '"';
    buf[n] = '\0';
    json_raw(buf);
}

static esp_err_t json_end(void)
{
    json_flush();
    return httpd_resp_send_chunk(s_json.req, NULL, 0);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Moves a join on once it has connected, failed or timed out
static void check_join(void)
{
    if (!s_joining) {
        return;
    }
    if (wifi_connected()) {
        s_joining = false;
        strlcpy(s_config.ssid, s_join_ssid, sizeof(s_config.ssid));
        strlcpy(s_config.password, s_join_password, sizeof(s_config.password));
        ESP_LOGI(TAG, "Joined \"%s\"", s_config.ssid);
    } else if (wifi_join_failed()) {
        s_joining = false;
        s_join_error = wifi_failure();
    } else if (xTaskGetTickCount() - s_join_started > pdMS_TO_TICKS(JOIN_TIMEOUT_MS)) {
        s_joining = false;
        wifi_stop_joining();
        s_join_error = "Couldn't connect";
    }
}

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    // EMBED_TXTFILES adds a terminating null, which isn't sent
    return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start - 1);
}

// {"ssid": "...", "connected": true, "joining": "", "error": "", "uprn": "...",
//  "networks": [{"ssid": "...", "rssi": -60, "open": false}, ...]}
static esp_err_t state_handler(httpd_req_t *req)
{
    check_join();
    char buf[64];
    json_begin(req);
    json_raw("{\"ssid\":");
    json_string(s_config.ssid);
    snprintf(buf, sizeof(buf), ",\"connected\":%s,\"joining\":", wifi_connected() && !s_joining ? "true" : "false");
    json_raw(buf);
    json_string(s_joining ? s_join_ssid : "");
    json_raw(",\"error\":");
    json_string(s_join_error);
    json_raw(",\"uprn\":");
    json_string(s_config.uprn);
    json_raw(",\"networks\":[");
    for (int i = 0; i < s_network_count; i++) {
        json_raw(i ? ",{\"ssid\":" : "{\"ssid\":");
        json_string((const char *)s_networks[i].ssid);
        snprintf(buf, sizeof(buf), ",\"rssi\":%d,\"open\":%s}", s_networks[i].rssi,
                 s_networks[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        json_raw(buf);
    }
    json_raw("]}");
    return json_end();
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    if (!s_joining) {
        s_network_count = wifi_scan(s_networks, MAX_NETWORKS);
    }
    return state_handler(req);
}

// POST ssid=...&password=...
static esp_err_t join_handler(httpd_req_t *req)
{
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        return send_json(req, "{\"error\":\"Bad request\"}");
    }
    form_value(body, "ssid", s_join_ssid, sizeof(s_join_ssid));
    form_value(body, "password", s_join_password, sizeof(s_join_password));
    if (!s_join_ssid[0]) {
        return send_json(req, "{\"error\":\"Choose a network\"}");
    }
    s_join_error = "";
    s_joining = true;
    s_join_started = xTaskGetTickCount();
    wifi_join(s_join_ssid, s_join_password, JOIN_RETRIES);
    return send_json(req, "{\"ok\":true}");
}

static void send_address(const char *uprn, const char *address, void *ctx)
{
    json_raw("{\"uprn\":");
    json_string(uprn);
    json_raw(",\"address\":");
    json_string(address);
    json_raw("},");
}

// GET ?postcode=... -> {"addresses": [{"uprn": "...", "address": "..."}, ...]}
static esp_err_t addresses_handler(httpd_req_t *req)
{
    char query[64], postcode[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        query[0] = '\0';
    }
    form_value(query, "postcode", postcode, sizeof(postcode));
    if (!postcode[0]) {
        return send_json(req, "{\"error\":\"Enter a postcode\"}");
    }
    if (!wifi_connected() || s_joining) {
        return send_json(req, "{\"error\":\"Connect to your WiFi first\"}");
    }
    json_begin(req);
    json_raw("{\"addresses\":[");
    int count = bin_address_lookup(postcode, send_address, NULL);
    // Every address ends with a comma, so finish the list with an empty
    // object that the page skips
    json_raw(count < 0 ? "{}],\"error\":\"Lookup failed, please try again\"}" : "{}]}");
    return json_end();
}

// POST uprn=...
static esp_err_t save_handler(httpd_req_t *req)
{
    char body[64], uprn[16];
    if (!read_body(req, body, sizeof(body))) {
        return send_json(req, "{\"error\":\"Bad request\"}");
    }
    form_value(body, "uprn", uprn, sizeof(uprn));
    size_t len = strlen(uprn);
    if (len == 0 || len > 12 || strspn(uprn, "0123456789") != len) {
        return send_json(req, "{\"error\":\"Choose your address, or enter a UPRN of up to 12 digits\"}");
    }
    check_join();
    if (!wifi_connected() || s_joining) {
        return send_json(req, "{\"error\":\"Connect to your WiFi first\"}");
    }
    // UPRNs are 12 digits with leading zeros
    memset(s_config.uprn, '0', 12 - len);
    strlcpy(s_config.uprn + 12 - len, uprn, sizeof(s_config.uprn) - (12 - len));
    if (!config_save(&s_config)) {
        return send_json(req, "{\"error\":\"Saving failed\"}");
    }
    s_saved = true;
    s_finished = true;
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t cancel_handler(httpd_req_t *req)
{
    s_finished = true;
    return send_json(req, "{\"ok\":true}");
}

// Anything else, including the addresses phones check for internet access,
// goes to the setup page. That's what makes the phone open it by itself.
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_ADDRESS "/");
    // iOS needs some content to spot the portal, not just the redirect
    return httpd_resp_sendstr(req, "Redirecting to setup");
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240; // the address lookup tries a TLS handshake
    config.lru_purge_enable = true;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_handler},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/join", .method = HTTP_POST, .handler = join_handler},
        {.uri = "/api/addresses", .method = HTTP_GET, .handler = addresses_handler},
        {.uri = "/api/save", .method = HTTP_POST, .handler = save_handler},
        {.uri = "/api/cancel", .method = HTTP_POST, .handler = cancel_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_handler);
}

// Newer phones also find the page from the DHCP captive portal option
static void set_dhcp_portal_option(void)
{
    static char uri[] = "http://" PORTAL_ADDRESS;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_dhcps_stop(netif);
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(netif);
}

void portal_start(const device_config_t *current)
{
    s_config = *current;

    // A name from the MAC address, and a new password each time. The
    // password leaves out characters that are easy to mix up.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "BinDay-%02X%02X", mac[4], mac[5]);
    static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) {
        s_password[i] = chars[esp_random() % (sizeof(chars) - 1)];
    }
    s_password[8] = '\0';

    // Quiet the web server's warnings, which the redirects make lots of, and
    // the DNS server's note of every lookup
    esp_log_level_set("example_dns_redirect_server", ESP_LOG_WARN);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    // Scan before the setup network starts, while nobody's on it to notice
    if (!wifi_connected()) {
        wifi_stop_joining();
    }
    s_network_count = wifi_scan(s_networks, MAX_NETWORKS);
    ESP_LOGI(TAG, "Found %d networks", s_network_count);

    wifi_start_setup_network(s_ssid, s_password);
    set_dhcp_portal_option();
    start_web_server();
    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&dns);
}

const char *portal_ssid(void)
{
    return s_ssid;
}

const char *portal_password(void)
{
    return s_password;
}

const char *portal_status(void)
{
    check_join();
    if (s_saved) {
        return "Saved, restarting...";
    }
    if (s_finished) {
        return "Cancelled, restarting...";
    }
    if (s_joining) {
        snprintf(s_status, sizeof(s_status), "Joining %s...", s_join_ssid);
        return s_status;
    }
    if (wifi_setup_clients() > 0) {
        return "Phone connected";
    }
    return "Waiting for your phone";
}

bool portal_finished(void)
{
    return s_finished;
}
