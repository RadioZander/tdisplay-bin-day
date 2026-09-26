// Downloading the bin calendar and looking up addresses, and keeping a copy
// of the calendar in NVS
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bin_calendar.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "bin_calendar";

// Both pages take a form POST: the postcode search (cal2.asp) returns the
// addresses and their UPRNs, and the calendar (cal_details.asp) needs the
// UPRN. Opening the calendar without one gives a 500 error.
//
// The council's HTTPS server only offers TLS 1.2 with RSA key exchange
// (AES256-SHA256), which Mbed TLS 4 (ESP-IDF 6) no longer supports. HTTPS is
// still tried first, so the device switches to it by itself if the council
// updates their server, and plain HTTP is the fallback.
#define SITE          "satellite.horsham.gov.uk/environment/refuse/"
#define CALENDAR_PAGE "cal_details.asp"
#define SEARCH_PAGE   "cal2.asp"

// The calendar is about 20 KB, and the postcode search about 14 KB plus
// 150 bytes per address
#define MAX_PAGE_SIZE (64 * 1024)

static const char *s_protocol;

#define NVS_NAMESPACE "bin_calendar"
#define NVS_KEY       "calendar"

// Reads the response body into a new, null-terminated buffer
static char *read_body(esp_http_client_handle_t client)
{
    char *buf = malloc(MAX_PAGE_SIZE + 1);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for the page");
        return NULL;
    }
    int len = 0;
    while (len < MAX_PAGE_SIZE) {
        int n = esp_http_client_read(client, buf + len, MAX_PAGE_SIZE - len);
        if (n < 0) {
            ESP_LOGE(TAG, "Read failed");
            free(buf);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += n;
    }
    buf[len] = '\0';
    ESP_LOGI(TAG, "Downloaded %d bytes", len);
    return buf;
}

typedef enum {
    FETCH_OK,
    FETCH_CONNECT_FAILED, // includes a failed TLS handshake
    FETCH_FAILED,
} fetch_result_t;

static fetch_result_t post_to(const char *url, const char *body, char **html)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return FETCH_FAILED;
    }
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    fetch_result_t result = FETCH_FAILED;
    int body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connecting to %s failed: %s", url, esp_err_to_name(err));
        result = FETCH_CONNECT_FAILED;
        goto done;
    }
    if (esp_http_client_write(client, body, body_len) != body_len) {
        ESP_LOGE(TAG, "Sending the request failed");
        goto done;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        goto done;
    }
    *html = read_body(client);
    if (*html) {
        result = FETCH_OK;
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

// POSTs a form to one of the council's pages, over HTTPS if possible and
// otherwise HTTP. Returns the page, which the caller frees, or NULL.
static char *post_form(const char *page, const char *body, const char **protocol)
{
    char url[96];
    char *html = NULL;
    snprintf(url, sizeof(url), "https://" SITE "%s", page);
    *protocol = "HTTPS";
    fetch_result_t result = post_to(url, body, &html);
    if (result == FETCH_CONNECT_FAILED) {
        ESP_LOGI(TAG, "Falling back to HTTP");
        snprintf(url, sizeof(url), "http://" SITE "%s", page);
        *protocol = "HTTP";
        post_to(url, body, &html);
    }
    return html;
}

int bin_calendar_fetch(const char *uprn, bin_calendar_t *cal)
{
    char body[32];
    snprintf(body, sizeof(body), "uprn=%s", uprn);
    const char *protocol;
    char *html = post_form(CALENDAR_PAGE, body, &protocol);
    if (!html) {
        return -1;
    }

    // Parse into a copy so a bad page leaves the old calendar alone
    static bin_calendar_t parsed;
    int count = bin_calendar_parse(html, &parsed);
    free(html);
    if (count == 0) {
        ESP_LOGW(TAG, "No collections found. Check the address in setup");
        return -1;
    }
    parsed.updated = time(NULL);
    *cal = parsed;
    s_protocol = protocol;
    ESP_LOGI(TAG, "Found %d collections", cal->count);
    return 0;
}

int bin_address_lookup(const char *postcode, bin_address_cb_t found, void *ctx)
{
    // Form-encode the postcode: "RH12 4QR" -> "RH12+4QR"
    char body[64] = "App=";
    size_t n = strlen(body);
    for (const char *p = postcode; *p && n < sizeof(body) - 24; p++) {
        if (isalnum((unsigned char)*p)) {
            body[n++] = *p;
        } else if (*p == ' ') {
            body[n++] = '+';
        } else {
            n += snprintf(body + n, sizeof(body) - n, "%%%02X", (unsigned char)*p);
        }
    }
    body[n] = '\0';
    strlcat(body, "&Submit=Search", sizeof(body));

    const char *protocol;
    char *html = post_form(SEARCH_PAGE, body, &protocol);
    if (!html) {
        return -1;
    }
    int count = bin_address_parse(html, found, ctx);
    free(html);
    ESP_LOGI(TAG, "Found %d addresses for %s", count, postcode);
    return count;
}

const char *bin_calendar_protocol(void)
{
    return s_protocol;
}

void bin_calendar_save(const bin_calendar_t *cal)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_blob(nvs, NVS_KEY, cal, sizeof(*cal));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't save the calendar: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

bool bin_calendar_load(bin_calendar_t *cal)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    // A blob of a different size was saved by an older version: ignore it
    size_t len = sizeof(*cal);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, cal, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len != sizeof(*cal)) {
        *cal = (bin_calendar_t){0};
        return false;
    }
    ESP_LOGI(TAG, "Loaded %d saved collections", cal->count);
    return true;
}
