// Board support: pins, panel geometry and screen layout for each supported
// LilyGO board. The board follows the ESP-IDF target, so `idf.py set-target
// esp32` builds for the T-Display and `idf.py set-target esp32s3` for the
// T-Display-S3.
#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

// LilyGO T-Display: 1.14" 135x240 ST7789 on SPI
#define BOARD_NAME         "T-Display"
#define BOARD_LCD_SPI      1
#define BOARD_PIN_MOSI     19
#define BOARD_PIN_SCLK     18
#define BOARD_PIN_CS       5
#define BOARD_PIN_DC       16
#define BOARD_PIN_RST      23
#define BOARD_PIN_BL       4
#define BOARD_LCD_PCLK_HZ  (40 * 1000 * 1000)

// Landscape, USB connector on the right. The 135x240 glass sits inside the
// ST7789's 240x320 RAM, hence the gap (see BOARD_LCD_RAM_*).
#define DISPLAY_WIDTH      240
#define DISPLAY_HEIGHT     135
#define BOARD_LCD_X_GAP    40
#define BOARD_LCD_Y_GAP    53
#define BOARD_LCD_MIRROR_X true
#define BOARD_LCD_MIRROR_Y false

// Both buttons have external pull-ups. GPIO35 is input-only with no
// internal pull-up.
#define BOARD_PIN_BUTTON_MENU   0
#define BOARD_PIN_BUTTON_CHANGE 35
#define BOARD_BUTTON_PULLUP     false

// Text scales for the 5x7 font: headline and body
#define BOARD_HEADLINE_SCALE 2
#define BOARD_BODY_SCALE     1

#elif CONFIG_IDF_TARGET_ESP32S3

// LilyGO T-Display-S3: 1.9" 170x320 ST7789 on an 8-bit parallel (i80) bus
#define BOARD_NAME         "T-Display-S3"
#define BOARD_LCD_I80      1
#define BOARD_PIN_DATA     {39, 40, 41, 42, 45, 46, 47, 48}
#define BOARD_PIN_WR       8
#define BOARD_PIN_RD       9  // unused by the driver, held high
#define BOARD_PIN_CS       6
#define BOARD_PIN_DC       7
#define BOARD_PIN_RST      5
#define BOARD_PIN_BL       38
#define BOARD_PIN_LCD_POWER 15 // must be high for the LCD to run on battery
#define BOARD_LCD_PCLK_HZ  (20 * 1000 * 1000)

// The backlight driver (AW9364) is dimmed by counting pulses on its enable
// pin rather than by PWM
#define BOARD_BL_PULSE_DIMMING 1

// Landscape. The 170x320 glass sits inside the ST7789's 240x320 RAM.
#define DISPLAY_WIDTH      320
#define DISPLAY_HEIGHT     170
#define BOARD_LCD_X_GAP    0
#define BOARD_LCD_Y_GAP    35
#define BOARD_LCD_MIRROR_X false
#define BOARD_LCD_MIRROR_Y true

#define BOARD_PIN_BUTTON_MENU   0
#define BOARD_PIN_BUTTON_CHANGE 14
#define BOARD_BUTTON_PULLUP     true

// Text scales for the 5x7 font: headline and body
#define BOARD_HEADLINE_SCALE 3
#define BOARD_BODY_SCALE     2

#else
#error "Unsupported target: use `idf.py set-target esp32` (T-Display) or `idf.py set-target esp32s3` (T-Display-S3)"
#endif

// Both boards use an ST7789, whose RAM is 320x240 in landscape. Rotating the
// screen 180 degrees moves the glass to the opposite side of the RAM, so the
// gap becomes whatever is left over on that side.
#define BOARD_LCD_RAM_WIDTH  320
#define BOARD_LCD_RAM_HEIGHT 240
