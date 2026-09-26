// WiFi: joining the home network, and the setup network the portal runs on
#pragma once

#include <stdbool.h>
#include "esp_wifi_types.h"

// Start WiFi in station mode, without joining anything yet
void wifi_init(void);

// Join a network, retrying up to `retries` times after a failure, or for
// ever if -1. Switches straight over if already connected elsewhere.
void wifi_join(const char *ssid, const char *password, int retries);

// Stop trying to join the network, but stay connected if already joined
void wifi_stop_joining(void);

bool wifi_connected(void);
const char *wifi_ip(void);        // "" when not connected
bool wifi_join_failed(void);      // ran out of retries
const char *wifi_failure(void);   // why, e.g. "Wrong password?"

// Start the setup network alongside the station, at 192.168.4.1
void wifi_start_setup_network(const char *ssid, const char *password);
int wifi_setup_clients(void);    // devices joined to the setup network

// Scan for networks, strongest first, one entry per name. Blocks for a
// couple of seconds. Returns the number found.
int wifi_scan(wifi_ap_record_t *records, int max);
