#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    BUTTON_MENU,   // GPIO0 (also the BOOT button)
    BUTTON_CHANGE, // GPIO35 on the T-Display, GPIO14 on the T-Display-S3
} button_id_t;

typedef struct {
    button_id_t button;
    bool long_press; // held for BUTTONS_LONG_PRESS_MS
} button_event_t;

#define BUTTONS_LONG_PRESS_MS 800

// Start a task that watches the board's two buttons and posts a
// button_event_t to the returned queue for each press. A short press is
// reported on release; a long press is reported as soon as the hold time is
// reached (and no short press follows it).
QueueHandle_t buttons_init(void);
