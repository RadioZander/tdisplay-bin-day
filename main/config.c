#include <string.h>
#include "config.h"
#include "esp_log.h"
#include "nvs.h"

// secrets.h is optional: it only gives the defaults for anything the setup
// portal hasn't saved
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef BIN_UPRN
#define BIN_UPRN ""
#endif

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "config";

static void load_str(nvs_handle_t nvs, const char *key, char *value, size_t size)
{
    size_t len = size;
    char saved[65];
    if (len > sizeof(saved)) {
        len = sizeof(saved);
    }
    if (nvs_get_str(nvs, key, saved, &len) == ESP_OK) {
        strlcpy(value, saved, size);
    }
}

void config_load(device_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->ssid, WIFI_SSID, sizeof(c->ssid));
    strlcpy(c->password, WIFI_PASSWORD, sizeof(c->password));
    strlcpy(c->uprn, BIN_UPRN, sizeof(c->uprn));
    // The example UPRN in secrets.example.h isn't a real property
    if (strspn(c->uprn, "0") == strlen(c->uprn)) {
        c->uprn[0] = '\0';
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return; // nothing saved yet
    }
    load_str(nvs, "ssid", c->ssid, sizeof(c->ssid));
    load_str(nvs, "password", c->password, sizeof(c->password));
    load_str(nvs, "uprn", c->uprn, sizeof(c->uprn));
    nvs_close(nvs);
}

bool config_save(const device_config_t *c)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    nvs_set_str(nvs, "ssid", c->ssid);
    nvs_set_str(nvs, "password", c->password);
    nvs_set_str(nvs, "uprn", c->uprn);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved setup for \"%s\"", c->ssid);
    return true;
}

bool config_complete(const device_config_t *c)
{
    return c->ssid[0] && c->uprn[0];
}
