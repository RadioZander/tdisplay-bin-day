// Setup portal: a WiFi network of the device's own, with a web page for
// choosing the home network and the property. Phones open the page by
// themselves when they join, as with hotel WiFi.
#pragma once

#include <stdbool.h>
#include "config.h"

#define PORTAL_ADDRESS "192.168.4.1"

// Start the setup network, DNS redirect and web server, starting from the
// current setup. Scans for networks first, which takes a couple of seconds.
void portal_start(const device_config_t *current);

// Shown on the screen: the setup network to join
const char *portal_ssid(void);
const char *portal_password(void);

// What's happening, in a few words, e.g. "Phone connected"
const char *portal_status(void);

// The page has saved the setup or cancelled it: time to restart
bool portal_finished(void);
