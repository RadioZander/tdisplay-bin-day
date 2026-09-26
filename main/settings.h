#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NIGHT_OFF,  // same brightness all the time
    NIGHT_DIM,  // lowest brightness overnight
    NIGHT_DARK, // backlight off overnight
    NUM_NIGHT_MODES,
} night_mode_t;

// User settings, changed from the on-device settings menu and kept in NVS
typedef struct {
    int brightness;        // backlight percent: 10, 25, 50, 75 or 100
    bool flipped;          // screen rotated 180 degrees
    night_mode_t night;
    int night_from;        // hour, 0-23
    int night_until;       // hour, 0-23
    int reminder_from;     // hour the day before a collection to start the reminder, 0 = all day
    uint32_t bins_out;     // date (yyyymmdd) of the collection whose bins are out, 0 = none
} bin_settings_t;

// Load saved settings, falling back to defaults for anything missing
void settings_load(bin_settings_t *s);
void settings_save(const bin_settings_t *s);

// Next (or previous) brightness level after `percent`, wrapping around
int settings_step_brightness(int percent, int direction);
