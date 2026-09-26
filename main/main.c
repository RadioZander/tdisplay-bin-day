// Bin day reminder for the LilyGO T-Display and T-Display-S3: downloads the
// property's collection calendar from Horsham District Council and shows the
// next collection, and which bins go out, on the built-in ST7789 LCD.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "bin_calendar.h"
#include "display.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing main/secrets.h - copy main/secrets.example.h to main/secrets.h and add your WiFi details and UPRN"
#endif

static const char *TAG = "bin_day";

#define REFRESH_INTERVAL_S (6 * 60 * 60) // after a successful download
#define PREVIEW_SCENE_S    5

#if CONFIG_BIN_PREVIEW
#define PREVIEW 1
#else
#define PREVIEW 0
#endif
#define RETRY_INTERVAL_S   (15 * 60)     // after a failed one

// Bin colours
#define COLOR_BIN_GREEN      RGB565(40, 160, 60)
#define COLOR_BIN_GREEN_DARK RGB565(20, 100, 40)
#define COLOR_BIN_BLUE       RGB565(40, 110, 230)
#define COLOR_BIN_BLUE_DARK  RGB565(25, 65, 150)
#define COLOR_BIN_ORANGE     RGB565(255, 140, 0)
#define COLOR_BIN_BROWN      RGB565(140, 85, 40)
#define COLOR_BIN_BODY       RGB565(70, 70, 70)

static const struct {
    uint8_t bin;
    const char *label;
    uint16_t body;
    uint16_t lid;
    bool caddy; // a small food caddy rather than a wheelie bin
} bin_styles[] = {
    {BIN_REFUSE, "Refuse", COLOR_BIN_GREEN_DARK, COLOR_BIN_GREEN, false},
    {BIN_RECYCLING, "Recycling", COLOR_BIN_BLUE_DARK, COLOR_BIN_BLUE, false},
    {BIN_FOOD, "Food", COLOR_BIN_BODY, COLOR_BIN_ORANGE, true},
    {BIN_GARDEN, "Garden", COLOR_BIN_BROWN, COLOR_BIN_BROWN, false},
    {BIN_OTHER, "Other", COLOR_GREY, COLOR_GREY, false},
};
#define NUM_BIN_STYLES (sizeof(bin_styles) / sizeof(bin_styles[0]))

static volatile bool s_wifi_connected;
static volatile bool s_time_synced;
static char s_ip[16] = "";

static bin_calendar_t s_calendar;
static bool s_fetch_failed;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        s_wifi_connected = true;
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to \"%s\"", WIFI_SSID);
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronised from %s", CONFIG_BIN_NTP_SERVER);
    s_time_synced = true;
}

static void sntp_init_client(void)
{
    // SNTP starts polling as soon as the network is up and re-syncs periodically
    // (CONFIG_LWIP_SNTP_UPDATE_DELAY, default 1 hour)
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_BIN_NTP_SERVER);
    config.sync_cb = time_sync_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

    setenv("TZ", CONFIG_BIN_TIMEZONE, 1);
    tzset();
}

static void draw_status_screen(int dots)
{
    char buf[32];
    display_clear(COLOR_BLACK);
    // Positions as a fraction of the screen height, to suit either board
    display_text_centered(DISPLAY_HEIGHT * 15 / 100, "Bin Day", 3, COLOR_CYAN);

    const char *msg = s_wifi_connected ? "Syncing time" : "Connecting WiFi";
    snprintf(buf, sizeof(buf), "%s%.*s", msg, dots, "...");
    display_text(12, DISPLAY_HEIGHT * 52 / 100, buf, 2, COLOR_WHITE);

    if (s_wifi_connected) {
        snprintf(buf, sizeof(buf), "IP %s", s_ip);
    } else {
        snprintf(buf, sizeof(buf), "SSID %s", WIFI_SSID);
    }
    display_text_centered(DISPLAY_HEIGHT * 82 / 100, buf, 1, COLOR_GREY);
    display_flush();
}

// A wheelie bin (or food caddy) `h` pixels tall, centred on `cx`
static void draw_bin(int cx, int top, int h, int style)
{
    uint16_t body = bin_styles[style].body;
    uint16_t lid = bin_styles[style].lid;
    if (bin_styles[style].caddy) {
        // Smaller, sitting on the same ground line, and no wheels
        int ch = h * 3 / 5;
        int w = ch;
        top += h - ch;
        display_fill_rect(cx - w / 2 - 2, top, w + 4, ch / 5, lid);
        display_fill_rect(cx - w / 2, top + ch / 5 + 1, w, ch - ch / 5 - 1, body);
        return;
    }
    int w = h * 2 / 3;
    int lid_h = h / 8;
    int wheel = h / 7;
    display_fill_rect(cx - w / 2 - h / 16, top, w + h / 8, lid_h, lid);
    display_fill_rect(cx - w / 2, top + lid_h + 1, w, h - lid_h - 1 - wheel / 2, body);
    display_fill_rect(cx - w / 2, top + h - wheel, wheel, wheel, COLOR_BLACK);
    display_fill_rect(cx - w / 2 + 1, top + h - wheel + 1, wheel - 2, wheel - 2, COLOR_GREY);
    display_fill_rect(cx + w / 2 - wheel, top + h - wheel, wheel, wheel, COLOR_BLACK);
    display_fill_rect(cx + w / 2 - wheel + 1, top + h - wheel + 1, wheel - 2, wheel - 2, COLOR_GREY);
}

// "Thu 1 Oct"
static void format_date(char *buf, size_t len, const bin_collection_t *c)
{
    struct tm date = {.tm_year = c->year - 1900, .tm_mon = c->month - 1, .tm_mday = c->day,
                      .tm_hour = 12, .tm_isdst = -1};
    mktime(&date); // fills in the weekday
    char weekday[8], month[8];
    strftime(weekday, sizeof(weekday), "%a", &date);
    strftime(month, sizeof(month), "%b", &date);
    snprintf(buf, len, "%s %d %s", weekday, c->day, month);
}

// "Refuse, Food"
static void format_bins(char *buf, size_t len, uint8_t bins)
{
    buf[0] = '\0';
    for (size_t i = 0; i < NUM_BIN_STYLES; i++) {
        if (bins & bin_styles[i].bin) {
            if (buf[0]) {
                strlcat(buf, ", ", len);
            }
            strlcat(buf, bin_styles[i].label, len);
        }
    }
}

static void draw_status_line(bool updating)
{
    char buf[40];
    display_text(4, 4, s_wifi_connected ? "WiFi OK" : "WiFi lost", 1, s_wifi_connected ? COLOR_GREEN : COLOR_RED);

    uint16_t color = COLOR_GREY;
    if (PREVIEW) {
        snprintf(buf, sizeof(buf), "Preview");
        color = COLOR_YELLOW;
    } else if (updating) {
        snprintf(buf, sizeof(buf), "Updating...");
    } else if (s_fetch_failed) {
        snprintf(buf, sizeof(buf), "Update failed");
        color = COLOR_RED;
    } else if (s_calendar.updated) {
        time_t updated = s_calendar.updated;
        struct tm tm;
        localtime_r(&updated, &tm);
        strftime(buf, sizeof(buf), "Updated %d %b %H:%M", &tm);
    } else {
        buf[0] = '\0';
    }
    display_text(DISPLAY_WIDTH - 4 - display_text_width(buf, 1), 4, buf, 1, color);
}

static void draw_screen(const struct tm *today, bool updating)
{
    char headline[32], subline[40], buf[64];
    display_clear(COLOR_BLACK);
    draw_status_line(updating);

    int headline_y = DISPLAY_HEIGHT * 14 / 100;
    int subline_y = DISPLAY_HEIGHT * 31 / 100;

    const bin_collection_t *next = bin_calendar_next(&s_calendar, today);
    if (!next) {
        display_text_centered(headline_y, "No collections", BOARD_HEADLINE_SCALE, COLOR_WHITE);
        const char *why = s_calendar.updated ? "None in the calendar" : "Waiting for the calendar";
        display_text_centered(subline_y, why, BOARD_BODY_SCALE, COLOR_GREY);
        display_flush();
        return;
    }

    char date[16];
    format_date(date, sizeof(date), next);
    int days = bin_collection_days_until(next, today);
    uint16_t headline_color = COLOR_YELLOW;
    if (days == 0) {
        snprintf(headline, sizeof(headline), "Collection today");
        snprintf(subline, sizeof(subline), "%s", date);
    } else if (days == 1) {
        snprintf(headline, sizeof(headline), "Bins out tonight");
        snprintf(subline, sizeof(subline), "For %s", date);
    } else {
        snprintf(headline, sizeof(headline), "%s", date);
        snprintf(subline, sizeof(subline), "In %d days", days);
        headline_color = COLOR_WHITE;
    }
    if (next->changed) {
        strlcat(subline, " (changed)", sizeof(subline));
    }
    display_text_centered(headline_y, headline, BOARD_HEADLINE_SCALE, headline_color);
    display_text_centered(subline_y, subline, BOARD_BODY_SCALE, next->changed ? COLOR_RED : COLOR_GREY);

    // One bin per slot across the screen, with its name underneath
    int count = 0;
    for (size_t i = 0; i < NUM_BIN_STYLES; i++) {
        count += (next->bins & bin_styles[i].bin) != 0;
    }
    int slot = DISPLAY_WIDTH / count;
    int bin_top = DISPLAY_HEIGHT * 42 / 100;
    int bin_h = DISPLAY_HEIGHT * 34 / 100;
    int label_y = bin_top + bin_h + 4;
    int x = 0;
    for (size_t i = 0; i < NUM_BIN_STYLES; i++) {
        if (!(next->bins & bin_styles[i].bin)) {
            continue;
        }
        int cx = x + slot / 2;
        draw_bin(cx, bin_top, bin_h, i);
        const char *label = bin_styles[i].label;
        int scale = display_text_width(label, BOARD_BODY_SCALE) <= slot - 4 ? BOARD_BODY_SCALE : 1;
        display_text(cx - display_text_width(label, scale) / 2, label_y, label, scale, COLOR_WHITE);
        x += slot;
    }

    // The collection after, in small print along the bottom
    const bin_collection_t *after = next + 1;
    if (after < s_calendar.collections + s_calendar.count) {
        char bins[40];
        format_date(date, sizeof(date), after);
        format_bins(bins, sizeof(bins), after->bins);
        snprintf(buf, sizeof(buf), "Then %s: %s", date, bins);
        display_text_centered(DISPLAY_HEIGHT - 12, buf, 1, after->changed ? COLOR_RED : COLOR_GREY);
    }

    display_flush();
}

#if CONFIG_BIN_PREVIEW
// Preview mode: replaces `today` with the day before, then the day of, each
// of the next two collections, moving on every PREVIEW_SCENE_S seconds
static void preview_date(time_t now, struct tm *today)
{
    const bin_collection_t *next = bin_calendar_next(&s_calendar, today);
    if (!next) {
        return;
    }
    int scene = (now / PREVIEW_SCENE_S) % 4;
    const bin_collection_t *c = next + scene / 2;
    if (c >= s_calendar.collections + s_calendar.count) {
        c = next;
    }
    struct tm date = {.tm_year = c->year - 1900, .tm_mon = c->month - 1,
                      .tm_mday = c->day - (scene % 2 == 0 ? 1 : 0), .tm_hour = 12, .tm_isdst = -1};
    mktime(&date); // normalises the day before the 1st
    *today = date;
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init(CONFIG_BIN_BRIGHTNESS);
    bin_calendar_load(&s_calendar);
    wifi_init();
    sntp_init_client();

    int dots = 0;
    while (!s_time_synced) {
        draw_status_screen(dots);
        dots = (dots + 1) % 4;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    time_t next_fetch = 0;
    int shown_step = -1;
    bool shown_wifi = false;
    while (true) {
        time_t now = time(NULL);
        struct tm today;
        localtime_r(&now, &today);

        if (now >= next_fetch && s_wifi_connected) {
            draw_screen(&today, true);
            if (bin_calendar_fetch(BIN_UPRN, &s_calendar) == 0) {
                bin_calendar_save(&s_calendar);
                s_fetch_failed = false;
                next_fetch = now + REFRESH_INTERVAL_S;
            } else {
                s_fetch_failed = true;
                next_fetch = now + RETRY_INTERVAL_S;
            }
            shown_step = -1;
        }

        // Redraw each minute, which also moves on to the next day at midnight
#if CONFIG_BIN_PREVIEW
        preview_date(now, &today);
        int step = now / PREVIEW_SCENE_S;
#else
        int step = today.tm_min;
#endif
        if (step != shown_step || s_wifi_connected != shown_wifi) {
            shown_step = step;
            shown_wifi = s_wifi_connected;
            draw_screen(&today, false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
