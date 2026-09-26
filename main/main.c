// Bin day reminder for the LilyGO T-Display and T-Display-S3: downloads the
// property's collection calendar from Horsham District Council and shows the
// next collection, and which bins go out, on the built-in ST7789 LCD.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "bin_calendar.h"
#include "buttons.h"
#include "config.h"
#include "display.h"
#include "portal.h"
#include "settings.h"
#include "wifi.h"


static const char *TAG = "bin_day";

#define REFRESH_INTERVAL_S (6 * 60 * 60) // after a successful download
#define RETRY_INTERVAL_S   (15 * 60)     // after a failed one
#define PREVIEW_SCENE_S    5
#define MENU_TIMEOUT_MS    10000
#define INFO_TIMEOUT_MS    60000 // the info page stays up longer, for reading
#define WAKE_MS            30000 // a button press lights a dark screen for this long
#define NIGHT_DIM_PERCENT  10
#define JOIN_TIMEOUT_MS    (2 * 60 * 1000)  // at start-up, before offering setup
#define PORTAL_IDLE_MS     (10 * 60 * 1000) // setup gives up if nobody joins
#define PORTAL_RESTART_MS  3000             // time to read the result before restarting

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

static volatile bool s_time_synced;
static device_config_t s_config;

static bin_calendar_t s_calendar;
static bool s_fetch_failed;
static bool s_updating;
static bool s_update_requested; // from the Update menu item
static time_t s_last_attempt;
static time_t s_next_fetch;

static bin_settings_t s_settings;
static bool s_preview; // not saved, so a restart always goes back to normal
static int s_brightness_shown = -1;
static TickType_t s_wake_until;

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
    char buf[48];
    display_clear(COLOR_BLACK);
    // Positions as a fraction of the screen height, to suit either board
    display_text_centered(DISPLAY_HEIGHT * 15 / 100, "Bin Day", 3, COLOR_CYAN);

    const char *msg = wifi_connected() ? "Syncing time" : "Connecting WiFi";
    snprintf(buf, sizeof(buf), "%s%.*s", msg, dots, "...");
    display_text(12, DISPLAY_HEIGHT * 52 / 100, buf, 2, COLOR_WHITE);

    if (wifi_connected()) {
        snprintf(buf, sizeof(buf), "IP %s", wifi_ip());
    } else {
        snprintf(buf, sizeof(buf), "SSID %s", s_config.ssid);
    }
    display_text_centered(DISPLAY_HEIGHT * 78 / 100, buf, 1, COLOR_GREY);
    display_text_centered(DISPLAY_HEIGHT - 12, "Hold MENU for setup", 1, COLOR_GREY);
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

// Where things stand with the next collection, for a given day
typedef struct {
    const bin_collection_t *next; // NULL if there are none
    int days;                     // until the next collection
    bool reminder;                // time to put the bins out, or collection day
    bool out;                     // the bins have been marked as out
} bin_status_t;

static bin_status_t get_status(const struct tm *today)
{
    bin_status_t st = {.next = bin_calendar_next(&s_calendar, today)};
    if (!st.next) {
        return st;
    }
    st.days = bin_collection_days_until(st.next, today);
    st.reminder = st.days == 0 || (st.days == 1 && today->tm_hour >= s_settings.reminder_from);
    st.out = st.days <= 1 && s_settings.bins_out == bin_collection_date_key(st.next);
    return st;
}

// Preview mode: replaces `today` with the evening before, then the day of,
// each of the next two collections, moving on every PREVIEW_SCENE_S seconds
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
    // 8pm, so the reminder shows whatever time it's set to start
    struct tm date = {.tm_year = c->year - 1900, .tm_mon = c->month - 1,
                      .tm_mday = c->day - (scene % 2 == 0 ? 1 : 0), .tm_hour = 20, .tm_isdst = -1};
    mktime(&date); // normalises the day before the 1st
    *today = date;
}

// On-device settings menu: long-press MENU to open or close (saves),
// short-press MENU for the next item, CHANGE to step the value forwards
// (long-press CHANGE steps backwards). Closes by itself after 10 s idle.
typedef enum {
    ITEM_BRIGHTNESS,
    ITEM_NIGHT,
    ITEM_NIGHT_FROM,
    ITEM_NIGHT_UNTIL,
    ITEM_REMINDER,
    ITEM_SCREEN,
    ITEM_PREVIEW,
    ITEM_UPDATE,
    ITEM_SETUP,
    ITEM_INFO,
    NUM_ITEMS,
} menu_item_t;

static const char *item_names[NUM_ITEMS] = {
    "Brightness", "Night", "Night from", "Night until", "Reminder", "Screen", "Preview", "Update", "Setup", "Info",
};
static const char *night_names[NUM_NIGHT_MODES] = {"Off", "Dim", "Screen off"};

static bool s_menu_open;
static menu_item_t s_menu_item;
static TickType_t s_menu_last_input;
static const char *s_update_result; // shown by the Update item after a manual update
static bool s_setup_requested;      // from the Setup menu item

static bool is_night(const struct tm *now)
{
    int from = s_settings.night_from;
    int until = s_settings.night_until;
    int hour = now->tm_hour;
    if (s_settings.night == NIGHT_OFF || from == until) {
        return false;
    }
    // The night usually runs over midnight, e.g. 23:00 to 06:00
    return from < until ? hour >= from && hour < until : hour >= from || hour < until;
}

// The night setting doesn't apply while the menu is open, for a while after
// a button press, or while the bins still need putting out
static int target_brightness(const struct tm *now, const bin_status_t *st)
{
    bool woken = (int32_t)(s_wake_until - xTaskGetTickCount()) > 0;
    if (s_menu_open || woken || !is_night(now) || (st->reminder && !st->out)) {
        return s_settings.brightness;
    }
    if (s_settings.night == NIGHT_DIM) {
        return s_settings.brightness < NIGHT_DIM_PERCENT ? s_settings.brightness : NIGHT_DIM_PERCENT;
    }
    return 0;
}

static void apply_brightness(int percent)
{
    if (percent != s_brightness_shown) {
        display_set_brightness(percent);
        s_brightness_shown = percent;
    }
}

static int step_hour(int hour, int direction)
{
    return (hour + direction + 24) % 24;
}

static void menu_step_value(int direction)
{
    switch (s_menu_item) {
    case ITEM_BRIGHTNESS:
        s_settings.brightness = settings_step_brightness(s_settings.brightness, direction);
        break;
    case ITEM_NIGHT:
        s_settings.night = (s_settings.night + direction + NUM_NIGHT_MODES) % NUM_NIGHT_MODES;
        break;
    case ITEM_NIGHT_FROM:
        s_settings.night_from = step_hour(s_settings.night_from, direction);
        break;
    case ITEM_NIGHT_UNTIL:
        s_settings.night_until = step_hour(s_settings.night_until, direction);
        break;
    case ITEM_REMINDER:
        s_settings.reminder_from = step_hour(s_settings.reminder_from, direction);
        break;
    case ITEM_SCREEN:
        s_settings.flipped = !s_settings.flipped;
        display_set_flipped(s_settings.flipped);
        break;
    case ITEM_PREVIEW:
        s_preview = !s_preview;
        break;
    case ITEM_UPDATE:
        if (!s_updating) {
            s_update_requested = true;
            s_update_result = NULL;
        }
        break;
    case ITEM_SETUP:
        s_setup_requested = direction > 0;
        break;
    default:
        break;
    }
}

static void menu_value_text(char *buf, size_t len)
{
    switch (s_menu_item) {
    case ITEM_BRIGHTNESS:
        snprintf(buf, len, "%d%%", s_settings.brightness);
        break;
    case ITEM_NIGHT:
        snprintf(buf, len, "%s", night_names[s_settings.night]);
        break;
    case ITEM_NIGHT_FROM:
        snprintf(buf, len, "%02d:00", s_settings.night_from);
        break;
    case ITEM_NIGHT_UNTIL:
        snprintf(buf, len, "%02d:00", s_settings.night_until);
        break;
    case ITEM_REMINDER:
        if (s_settings.reminder_from == 0) {
            snprintf(buf, len, "All day");
        } else {
            snprintf(buf, len, "From %02d:00", s_settings.reminder_from);
        }
        break;
    case ITEM_SCREEN:
        snprintf(buf, len, "%s", s_settings.flipped ? "Flipped" : "Normal");
        break;
    case ITEM_PREVIEW:
        snprintf(buf, len, "%s", s_preview ? "On" : "Off");
        break;
    case ITEM_UPDATE:
        snprintf(buf, len, "%s", s_updating ? "Updating..." : s_update_result ? s_update_result : "Press CHANGE");
        break;
    case ITEM_SETUP:
        snprintf(buf, len, "Press CHANGE");
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void menu_close(void)
{
    s_menu_open = false;
    settings_save(&s_settings);
    ESP_LOGI(TAG, "Settings menu closed");
}

static bool menu_timed_out(void)
{
    int timeout = s_menu_item == ITEM_INFO ? INFO_TIMEOUT_MS : MENU_TIMEOUT_MS;
    return s_menu_open && xTaskGetTickCount() - s_menu_last_input > pdMS_TO_TICKS(timeout);
}

// `st` is the status on screen when the button was pressed
static void handle_button(const button_event_t *ev, const bin_status_t *st)
{
    s_menu_last_input = xTaskGetTickCount();

    // While the screen is dark, a press only lights it up
    if (s_brightness_shown == 0 && !s_menu_open) {
        s_wake_until = xTaskGetTickCount() + pdMS_TO_TICKS(WAKE_MS);
        return;
    }

    if (!s_menu_open) {
        if (ev->button == BUTTON_MENU && ev->long_press) {
            s_menu_open = true;
            s_menu_item = ITEM_BRIGHTNESS;
            s_update_result = NULL;
            ESP_LOGI(TAG, "Settings menu opened");
        } else if (ev->button == BUTTON_CHANGE && !ev->long_press && st->next && st->days <= 1) {
            // Mark the bins as out, or not out if pressed by mistake
            s_settings.bins_out = st->out ? 0 : bin_collection_date_key(st->next);
            settings_save(&s_settings);
            ESP_LOGI(TAG, "Bins %s", st->out ? "not out" : "out");
        }
        return;
    }

    if (ev->button == BUTTON_MENU) {
        if (ev->long_press) {
            menu_close();
        } else {
            s_menu_item = (s_menu_item + 1) % NUM_ITEMS;
            s_update_result = NULL;
        }
    } else {
        menu_step_value(ev->long_press ? -1 : 1);
    }

    if (s_menu_open) {
        char value[24];
        menu_value_text(value, sizeof(value));
        ESP_LOGI(TAG, "%s: %s", item_names[s_menu_item], value);
    }
}

static void draw_menu_bar(void)
{
    char value[24], buf[40];
    menu_value_text(value, sizeof(value));
    if (value[0]) {
        snprintf(buf, sizeof(buf), "%s: %s", item_names[s_menu_item], value);
    } else {
        snprintf(buf, sizeof(buf), "%s", item_names[s_menu_item]);
    }
    int scale = display_text_width(buf, 2) <= DISPLAY_WIDTH - 8 ? 2 : 1;
    display_fill_rect(0, 0, DISPLAY_WIDTH, 26, COLOR_YELLOW);
    display_text_centered(13 - 4 * scale, buf, scale, COLOR_BLACK);
}

static void draw_status_line(void)
{
    char buf[40];
    display_text(4, 4, wifi_connected() ? "WiFi OK" : "WiFi lost", 1, wifi_connected() ? COLOR_GREEN : COLOR_RED);

    uint16_t color = COLOR_GREY;
    if (s_preview) {
        snprintf(buf, sizeof(buf), "Preview");
        color = COLOR_YELLOW;
    } else if (s_updating) {
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

// The Info menu item: connection and update details, in place of the bins
static void draw_info(void)
{
    char lines[8][32];
    int n = 0;
    struct tm tm;

    if (wifi_connected()) {
        snprintf(lines[n++], sizeof(lines[0]), "IP %s", wifi_ip());
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(lines[n++], sizeof(lines[0]), "Signal %d dBm", ap.rssi);
        }
    } else {
        snprintf(lines[n++], sizeof(lines[0]), "WiFi not connected");
    }

    if (s_calendar.updated) {
        time_t updated = s_calendar.updated;
        localtime_r(&updated, &tm);
        strftime(lines[n++], sizeof(lines[0]), "Updated %d %b %H:%M", &tm);
    } else {
        snprintf(lines[n++], sizeof(lines[0]), "Never updated");
    }
    if (s_last_attempt) {
        const char *protocol = bin_calendar_protocol();
        if (s_fetch_failed) {
            snprintf(lines[n++], sizeof(lines[0]), "Last try failed");
        } else {
            snprintf(lines[n++], sizeof(lines[0]), "Last try OK (%s)", protocol ? protocol : "?");
        }
        localtime_r(&s_next_fetch, &tm);
        strftime(lines[n++], sizeof(lines[0]), "Next try %H:%M", &tm);
    }
    snprintf(lines[n++], sizeof(lines[0]), "%d collections saved", s_calendar.count);
    snprintf(lines[n++], sizeof(lines[0]), "Built %s", esp_app_get_description()->date);

    int line_h = 9 * BOARD_BODY_SCALE + 1;
    for (int i = 0; i < n; i++) {
        display_text(6, 32 + i * line_h, lines[i], BOARD_BODY_SCALE, COLOR_WHITE);
    }
}

// The next collection and its bins
static void draw_collection(const bin_status_t *st)
{
    char headline[32], subline[40], buf[64], date[16];
    int headline_y = DISPLAY_HEIGHT * 14 / 100;
    int subline_y = DISPLAY_HEIGHT * 31 / 100;

    const bin_collection_t *next = st->next;
    if (!next) {
        display_text_centered(headline_y, "No collections", BOARD_HEADLINE_SCALE, COLOR_WHITE);
        const char *why = s_calendar.updated ? "None in the calendar" : "Waiting for the calendar";
        display_text_centered(subline_y, why, BOARD_BODY_SCALE, COLOR_GREY);
        return;
    }

    format_date(date, sizeof(date), next);
    uint16_t headline_color = COLOR_WHITE;
    if (st->out) {
        snprintf(headline, sizeof(headline), "Bins are out");
        if (st->days == 0) {
            snprintf(subline, sizeof(subline), "Collection today");
        } else {
            snprintf(subline, sizeof(subline), "For %s", date);
        }
        headline_color = COLOR_GREEN;
    } else if (st->days == 0) {
        snprintf(headline, sizeof(headline), "Collection today");
        snprintf(subline, sizeof(subline), "%s", date);
        headline_color = COLOR_YELLOW;
    } else if (st->days == 1 && st->reminder) {
        snprintf(headline, sizeof(headline), "Bins out tonight");
        snprintf(subline, sizeof(subline), "For %s", date);
        headline_color = COLOR_YELLOW;
    } else if (st->days == 1) {
        snprintf(headline, sizeof(headline), "Tomorrow");
        snprintf(subline, sizeof(subline), "%s", date);
    } else {
        snprintf(headline, sizeof(headline), "%s", date);
        snprintf(subline, sizeof(subline), "In %d days", st->days);
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

    // Along the bottom: how to mark the bins as out while they need doing,
    // otherwise the collection after this one
    const bin_collection_t *after = next + 1;
    if (st->reminder && !st->out) {
        display_text_centered(DISPLAY_HEIGHT - 12, "Press CHANGE when they're out", 1, COLOR_YELLOW);
    } else if (after < s_calendar.collections + s_calendar.count) {
        char bins[40];
        format_date(date, sizeof(date), after);
        format_bins(bins, sizeof(bins), after->bins);
        snprintf(buf, sizeof(buf), "Then %s: %s", date, bins);
        display_text_centered(DISPLAY_HEIGHT - 12, buf, 1, after->changed ? COLOR_RED : COLOR_GREY);
    }
}

static void draw_screen(const bin_status_t *st)
{
    display_clear(COLOR_BLACK);
    if (s_menu_open) {
        draw_menu_bar();
    } else {
        draw_status_line();
    }
    if (s_menu_open && s_menu_item == ITEM_INFO) {
        draw_info();
    } else {
        draw_collection(st);
    }
    display_flush();
}

// The setup network's name and password get typed in on a phone, so they're
// never smaller than 2x, even on the T-Display
#define PORTAL_CREDENTIALS_SCALE (BOARD_BODY_SCALE > 2 ? BOARD_BODY_SCALE : 2)

static void draw_portal_screen(const char *reason, const char *status)
{
    char buf[40];
    display_clear(COLOR_BLACK);
    display_text(4, 4, reason, 1, COLOR_GREY);
    if (config_complete(&s_config)) {
        const char *cancel = "Hold MENU to cancel";
        display_text(DISPLAY_WIDTH - 4 - display_text_width(cancel, 1), 4, cancel, 1, COLOR_GREY);
    }
    display_text_centered(DISPLAY_HEIGHT * 12 / 100, "Setup", BOARD_HEADLINE_SCALE, COLOR_CYAN);
    if (!portal_ssid()[0]) {
        display_text_centered(DISPLAY_HEIGHT * 45 / 100, "Starting...", BOARD_BODY_SCALE, COLOR_WHITE);
        display_flush();
        return;
    }
    display_text_centered(DISPLAY_HEIGHT * 32 / 100, "On your phone, join the WiFi network", 1, COLOR_GREY);
    display_text_centered(DISPLAY_HEIGHT * 41 / 100, portal_ssid(), PORTAL_CREDENTIALS_SCALE, COLOR_YELLOW);
    snprintf(buf, sizeof(buf), "Password %s", portal_password());
    display_text_centered(DISPLAY_HEIGHT * 54 / 100, buf, PORTAL_CREDENTIALS_SCALE, COLOR_WHITE);
    display_text_centered(DISPLAY_HEIGHT * 68 / 100, "The setup page opens by itself,", 1, COLOR_GREY);
    display_text_centered(DISPLAY_HEIGHT * 75 / 100, "or go to http://" PORTAL_ADDRESS, 1, COLOR_GREY);
    display_text_centered(DISPLAY_HEIGHT - 16, status, 1, COLOR_GREEN);
    display_flush();
}

// Runs the setup portal until the page saves or cancels, then restarts.
// `give_up` restarts it (to try the saved network again) if nobody joins
// the setup network for PORTAL_IDLE_MS. Never returns.
static void run_portal(const char *reason, bool give_up, QueueHandle_t buttons)
{
    ESP_LOGI(TAG, "Setup: %s", reason);
    apply_brightness(s_settings.brightness);
    draw_portal_screen(reason, "");
    portal_start(&s_config);

    TickType_t last_joined = xTaskGetTickCount();
    TickType_t finished_at = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        draw_portal_screen(reason, portal_status());
        if (wifi_setup_clients() > 0) {
            last_joined = now;
        }
        if (portal_finished() && !finished_at) {
            finished_at = now;
        }
        if (finished_at && now - finished_at > pdMS_TO_TICKS(PORTAL_RESTART_MS)) {
            esp_restart();
        }
        if (give_up && now - last_joined > pdMS_TO_TICKS(PORTAL_IDLE_MS)) {
            ESP_LOGI(TAG, "Nobody joined the setup network, restarting");
            esp_restart();
        }
        button_event_t ev;
        if (xQueueReceive(buttons, &ev, pdMS_TO_TICKS(500)) && ev.button == BUTTON_MENU && ev.long_press &&
            config_complete(&s_config)) {
            ESP_LOGI(TAG, "Setup cancelled");
            esp_restart();
        }
    }
}

static void update_calendar(time_t now)
{
    s_updating = true;
    bool ok = bin_calendar_fetch(s_config.uprn, &s_calendar) == 0;
    s_updating = false;
    s_last_attempt = now;
    s_fetch_failed = !ok;
    s_next_fetch = now + (ok ? REFRESH_INTERVAL_S : RETRY_INTERVAL_S);
    if (ok) {
        bin_calendar_save(&s_calendar);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    settings_load(&s_settings);
    config_load(&s_config);
    display_init(s_settings.brightness);
    display_set_flipped(s_settings.flipped);
    s_brightness_shown = s_settings.brightness;
    bin_calendar_load(&s_calendar);
    wifi_init();
    sntp_init_client();
    QueueHandle_t buttons = buttons_init();

    if (!config_complete(&s_config)) {
        run_portal("First-time setup", false, buttons);
    }

    // Offer setup if the saved network can't be joined, or on a long press
    // of MENU
    wifi_join(s_config.ssid, s_config.password, -1);
    TickType_t joining_since = xTaskGetTickCount();
    int dots = 0;
    while (!s_time_synced) {
        char reason[48];
        if (!wifi_connected() && xTaskGetTickCount() - joining_since > pdMS_TO_TICKS(JOIN_TIMEOUT_MS)) {
            snprintf(reason, sizeof(reason), "Can't join %s", s_config.ssid);
            run_portal(reason, true, buttons);
        }
        draw_status_screen(dots);
        dots = (dots + 1) % 4;
        button_event_t ev;
        if (xQueueReceive(buttons, &ev, pdMS_TO_TICKS(500)) && ev.button == BUTTON_MENU && ev.long_press) {
            run_portal("Setup", true, buttons);
        }
    }
    bin_status_t shown = {0};
    int shown_step = -1;
    bool shown_wifi = false;
    while (true) {
        bool redraw = false;

        // Wait a little for a button, then check everything else
        button_event_t ev;
        if (xQueueReceive(buttons, &ev, pdMS_TO_TICKS(50))) {
            handle_button(&ev, &shown);
            redraw = true;
        } else if (menu_timed_out()) {
            menu_close();
            redraw = true;
        }
        if (s_setup_requested) {
            menu_close();
            run_portal("Setup", true, buttons);
        }

        time_t now = time(NULL);
        struct tm real_today;
        localtime_r(&now, &real_today);
        struct tm today = real_today;
        if (s_preview) {
            preview_date(now, &today);
        }
        bin_status_t st = get_status(&today);

        if (s_update_requested && !wifi_connected()) {
            s_update_requested = false;
            s_update_result = "No WiFi";
            redraw = true;
        }
        if (wifi_connected() && (now >= s_next_fetch || s_update_requested)) {
            bool manual = s_update_requested;
            s_update_requested = false;
            s_updating = true;
            draw_screen(&st);
            update_calendar(now);
            if (manual) {
                s_update_result = s_fetch_failed ? "Failed" : "Done";
                s_menu_last_input = xTaskGetTickCount(); // time to read the result
            }
            st = get_status(&today);
            redraw = true;
        }

        // Redraw each minute, which also moves on to the next day at
        // midnight, or each scene in preview mode
        int step = s_preview ? now / PREVIEW_SCENE_S : real_today.tm_min;
        if (redraw || step != shown_step || wifi_connected() != shown_wifi) {
            shown_step = step;
            shown_wifi = wifi_connected();
            draw_screen(&st);
            shown = st;
        }

        // Night dimming follows the real time, even in preview mode
        bin_status_t real_st = s_preview ? get_status(&real_today) : st;
        apply_brightness(target_brightness(&real_today, &real_st));
    }
}
