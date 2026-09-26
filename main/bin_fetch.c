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

// Pages are parsed a piece at a time as they download, so the whole page
// (about 20 KB) is never held in memory. That matters on the original
// T-Display, where there's no 20 KB block free while setup is running.
#define PIECE_SIZE 4096

static const char *s_protocol;

#define NVS_NAMESPACE "bin_calendar"
#define NVS_KEY       "calendar"

typedef struct {
    esp_http_client_handle_t client;
    int total;
} http_reader_t;

static int read_http(char *buf, int size, void *ctx)
{
    http_reader_t *r = ctx;
    int n = esp_http_client_read(r->client, buf, size);
    if (n > 0) {
        r->total += n;
    }
    return n;
}

typedef enum {
    FETCH_OK,
    FETCH_CONNECT_FAILED, // includes a failed TLS handshake
    FETCH_FAILED,
} fetch_result_t;

static fetch_result_t post_to(const char *url, const char *body, bin_parse_fn parse, void *parse_ctx)
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
    char *buf = NULL;
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
    buf = malloc(PIECE_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for reading the page");
        goto done;
    }
    http_reader_t reader = {.client = client};
    if (!bin_stream_parse(buf, PIECE_SIZE, read_http, &reader, parse, parse_ctx)) {
        ESP_LOGE(TAG, "Read failed");
        goto done;
    }
    ESP_LOGI(TAG, "Downloaded %d bytes", reader.total);
    result = FETCH_OK;

done:
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

// POSTs a form to one of the council's pages, over HTTPS if possible and
// otherwise HTTP, parsing the page as it arrives. Returns false if that
// failed.
static bool post_form(const char *page, const char *body, bin_parse_fn parse, void *parse_ctx, const char **protocol)
{
    char url[96];
    snprintf(url, sizeof(url), "https://" SITE "%s", page);
    *protocol = "HTTPS";
    // A failed connection happens before any of the page is read, so it's
    // safe to try again over HTTP with the same parser
    fetch_result_t result = post_to(url, body, parse, parse_ctx);
    if (result == FETCH_CONNECT_FAILED) {
        ESP_LOGI(TAG, "Falling back to HTTP");
        snprintf(url, sizeof(url), "http://" SITE "%s", page);
        *protocol = "HTTP";
        result = post_to(url, body, parse, parse_ctx);
    }
    return result == FETCH_OK;
}

static size_t parse_calendar_piece(const char *text, void *ctx)
{
    return bin_calendar_parse_more(text, ctx);
}

int bin_calendar_fetch(const char *uprn, bin_calendar_t *cal)
{
    char body[32];
    snprintf(body, sizeof(body), "uprn=%s", uprn);

    // Parse into a copy so a bad page leaves the old calendar alone
    static bin_calendar_t parsed;
    parsed.count = 0;
    const char *protocol;
    if (!post_form(CALENDAR_PAGE, body, parse_calendar_piece, &parsed, &protocol)) {
        return -1;
    }
    if (parsed.count == 0) {
        ESP_LOGW(TAG, "No collections found. Check the address in setup");
        return -1;
    }
    parsed.updated = time(NULL);
    *cal = parsed;
    s_protocol = protocol;
    ESP_LOGI(TAG, "Found %d collections", cal->count);
    return 0;
}

typedef struct {
    bin_address_cb_t found;
    void *ctx;
    int count;
} address_lookup_t;

static size_t parse_address_piece(const char *text, void *ctx)
{
    address_lookup_t *lookup = ctx;
    return bin_address_parse_more(text, lookup->found, lookup->ctx, &lookup->count);
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

    address_lookup_t lookup = {.found = found, .ctx = ctx};
    const char *protocol;
    if (!post_form(SEARCH_PAGE, body, parse_address_piece, &lookup, &protocol)) {
        return -1;
    }
    ESP_LOGI(TAG, "Found %d addresses", lookup.count);
    return lookup.count;
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
