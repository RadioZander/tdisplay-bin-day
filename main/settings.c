#include "settings.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "settings";

static const int brightness_levels[] = {10, 25, 50, 75, 100};
#define NUM_LEVELS (int)(sizeof(brightness_levels) / sizeof(brightness_levels[0]))

// Reads a u8 into `*value` if it's saved and no more than `max`
static void load_u8(nvs_handle_t nvs, const char *key, int *value, int max)
{
    uint8_t v;
    if (nvs_get_u8(nvs, key, &v) == ESP_OK && v <= max) {
        *value = v;
    }
}

void settings_load(bin_settings_t *s)
{
    *s = (bin_settings_t) {
        .brightness = 75,
        .flipped = false,
        .night = NIGHT_OFF,
        .night_from = 23,
        .night_until = 6,
        .reminder_from = 0,
        .bins_out = 0,
    };

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return; // nothing saved yet
    }
    int flipped = s->flipped;
    int night = s->night;
    load_u8(nvs, "brightness", &s->brightness, 100);
    load_u8(nvs, "flipped", &flipped, 1);
    load_u8(nvs, "night", &night, NUM_NIGHT_MODES - 1);
    load_u8(nvs, "night_from", &s->night_from, 23);
    load_u8(nvs, "night_until", &s->night_until, 23);
    load_u8(nvs, "reminder", &s->reminder_from, 23);
    s->flipped = flipped;
    s->night = night;
    uint32_t bins_out;
    if (nvs_get_u32(nvs, "bins_out", &bins_out) == ESP_OK) {
        s->bins_out = bins_out;
    }
    nvs_close(nvs);
}

void settings_save(const bin_settings_t *s)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u8(nvs, "brightness", s->brightness);
    nvs_set_u8(nvs, "flipped", s->flipped);
    nvs_set_u8(nvs, "night", s->night);
    nvs_set_u8(nvs, "night_from", s->night_from);
    nvs_set_u8(nvs, "night_until", s->night_until);
    nvs_set_u8(nvs, "reminder", s->reminder_from);
    nvs_set_u32(nvs, "bins_out", s->bins_out);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved");
    }
}

int settings_step_brightness(int percent, int direction)
{
    // Find the current level (or the nearest one above it) and step from there
    int i = 0;
    while (i < NUM_LEVELS - 1 && brightness_levels[i] < percent) {
        i++;
    }
    i = (i + direction + NUM_LEVELS) % NUM_LEVELS;
    return brightness_levels[i];
}
