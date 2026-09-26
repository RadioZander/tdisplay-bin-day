// Polled, debounced buttons. Both buttons read low when pressed.
#include "buttons.h"
#include "board.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define POLL_MS     10
#define DEBOUNCE_MS 30

static const gpio_num_t pins[] = {
    [BUTTON_MENU] = BOARD_PIN_BUTTON_MENU,
    [BUTTON_CHANGE] = BOARD_PIN_BUTTON_CHANGE,
};
#define NUM_BUTTONS (sizeof(pins) / sizeof(pins[0]))

typedef struct {
    bool pressed;   // debounced state
    int stable_ms;  // how long the raw reading has differed from `pressed`
    int held_ms;    // how long the button has been held down
    bool long_sent; // long press already reported for this hold
} button_state_t;

static QueueHandle_t s_queue;

static void send(button_id_t button, bool long_press)
{
    button_event_t ev = {.button = button, .long_press = long_press};
    xQueueSend(s_queue, &ev, 0);
}

static void buttons_task(void *arg)
{
    button_state_t state[NUM_BUTTONS] = {0};

    while (true) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            button_state_t *b = &state[i];
            bool down = gpio_get_level(pins[i]) == 0;

            if (b->pressed) {
                b->held_ms += POLL_MS;
                if (!b->long_sent && b->held_ms >= BUTTONS_LONG_PRESS_MS) {
                    b->long_sent = true;
                    send(i, true);
                }
            }

            if (down == b->pressed) {
                b->stable_ms = 0;
                continue;
            }
            // Only accept a change once it has been steady for DEBOUNCE_MS
            b->stable_ms += POLL_MS;
            if (b->stable_ms < DEBOUNCE_MS) {
                continue;
            }
            b->pressed = down;
            b->stable_ms = 0;
            if (down) {
                b->held_ms = 0;
                b->long_sent = false;
            } else if (!b->long_sent) {
                send(i, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

QueueHandle_t buttons_init(void)
{
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_BUTTON_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    for (int i = 0; i < NUM_BUTTONS; i++) {
        cfg.pin_bit_mask |= 1ULL << pins[i];
    }
    ESP_ERROR_CHECK(gpio_config(&cfg));

    s_queue = xQueueCreate(8, sizeof(button_event_t));
    assert(s_queue);
    xTaskCreate(buttons_task, "buttons", 2048, NULL, 5, NULL);
    return s_queue;
}
