#pragma once

#include <stdbool.h>

// The device's setup: which WiFi network to join and which property's bins
// to show. Saved in NVS by the setup portal.
typedef struct {
    char ssid[33];
    char password[65];
    char uprn[16];
} device_config_t;

// Load the saved setup. Anything not saved falls back to secrets.h, if
// there is one, otherwise it's left empty.
void config_load(device_config_t *c);
bool config_save(const device_config_t *c);

// Setup is complete: there's a network to join and a property to show
bool config_complete(const device_config_t *c);
