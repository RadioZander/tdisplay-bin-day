#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static volatile bool s_started;
static volatile bool s_want_join; // join once the station has started
static volatile bool s_connected;
static volatile bool s_switching; // disconnecting on purpose, to join another network
static volatile int s_retries;
static volatile bool s_failed;
static volatile uint8_t s_reason;
static volatile int s_setup_clients;
static char s_ip[16] = "";

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_want_join) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        s_connected = false;
        s_ip[0] = '\0';
        if (s_switching) {
            s_switching = false;
            esp_wifi_connect();
            return;
        }
        s_reason = event->reason;
        if (s_retries != 0) {
            if (s_retries > 0) {
                s_retries--;
            }
            ESP_LOGW(TAG, "Disconnected (reason %d), retrying...", event->reason);
            esp_wifi_connect();
        } else if (s_want_join) {
            ESP_LOGW(TAG, "Couldn't connect (reason %d)", event->reason);
            s_want_join = false;
            s_failed = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        s_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_setup_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        s_setup_clients = s_setup_clients > 0 ? s_setup_clients - 1 : 0;
    }
}

void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // The network details are kept in our own config, so the WiFi driver
    // doesn't need to save them too
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_join(const char *ssid, const char *password, int retries)
{
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));

    s_retries = retries;
    s_failed = false;
    s_want_join = true;
    ESP_LOGI(TAG, "Connecting to \"%s\"", ssid);
    if (s_connected) {
        // The disconnect event then joins the new network. Not connected from
        // here on, so nobody mistakes the old connection for the new one.
        s_switching = true;
        s_connected = false;
        s_ip[0] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &config);
        esp_wifi_disconnect();
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &config);
    if (s_started) {
        esp_wifi_connect();
    }
}

void wifi_stop_joining(void)
{
    s_retries = 0;
    s_want_join = false;
    if (!s_connected) {
        esp_wifi_disconnect();
    }
}

bool wifi_connected(void)
{
    return s_connected;
}

const char *wifi_ip(void)
{
    return s_ip;
}

bool wifi_join_failed(void)
{
    return s_failed;
}

const char *wifi_failure(void)
{
    switch (s_reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Wrong password?";
    default:
        return "Couldn't connect";
    }
}

void wifi_start_setup_network(const char *ssid, const char *password)
{
    wifi_config_t config = {
        .ap = {
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)config.ap.ssid, ssid, sizeof(config.ap.ssid));
    config.ap.ssid_len = strlen(ssid);
    strlcpy((char *)config.ap.password, password, sizeof(config.ap.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_LOGI(TAG, "Setup network \"%s\" started", ssid);
}

int wifi_setup_clients(void)
{
    return s_setup_clients;
}

static int by_signal(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int wifi_scan(wifi_ap_record_t *records, int max)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    static wifi_ap_record_t found[32];
    uint16_t got = n < 32 ? n : 32;
    if (esp_wifi_scan_get_ap_records(&got, found) != ESP_OK) {
        return 0;
    }
    // Strongest first, skipping hidden networks and repeats of the same name
    // (e.g. mesh access points)
    qsort(found, got, sizeof(found[0]), by_signal);
    int count = 0;
    for (int i = 0; i < got && count < max; i++) {
        if (found[i].ssid[0] == '\0') {
            continue;
        }
        bool repeat = false;
        for (int j = 0; j < count && !repeat; j++) {
            repeat = strcmp((char *)records[j].ssid, (char *)found[i].ssid) == 0;
        }
        if (!repeat) {
            records[count++] = found[i];
        }
    }
    return count;
}
