// Minimal ST7789 driver for the LilyGO T-Display (SPI) and T-Display-S3 (8-bit i80)
#include <math.h>
#include <string.h>
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#if BOARD_LCD_SPI
#include "driver/spi_master.h"
#define LCD_HOST SPI2_HOST
#endif

static const char *TAG = "display";

#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_DUTY_BITS    LEDC_TIMER_10_BIT
#define BL_DUTY_MAX     ((1 << 10) - 1)
#define BL_PULSE_STEPS  16 // AW9364 brightness levels

#define FB_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t))

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static SemaphoreHandle_t s_flush_done;

// Called from the LCD ISR once the whole frame has been sent
static bool on_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

// Classic 5x7 font from Adafruit GFX (glcdfont.c, BSD licence, see README),
// ASCII 0x20..0x7E. Five column bytes per glyph, LSB = top row.
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06},
    {0x3E, 0x41, 0x5D, 0x59, 0x4E}, {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x73},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32},
    {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40}, {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28},
    {0x38, 0x44, 0x44, 0x28, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0xFC, 0x18, 0x24, 0x24, 0x18}, {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x77, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

static void backlight_init(void)
{
#if BOARD_BL_PULSE_DIMMING
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << BOARD_PIN_BL, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(BOARD_PIN_BL, 0);
#else
    // Backlight is PWM-dimmed; start dark until the first frame is drawn
    ledc_timer_config_t bl_timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_DUTY_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_channel = {
        .gpio_num = BOARD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_channel));
#endif
}

#if BOARD_LCD_SPI
static esp_lcd_panel_io_handle_t panel_io_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_PIN_SCLK,
        .mosi_io_num = BOARD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_PIN_CS,
        .dc_gpio_num = BOARD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_flush_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));
    return io;
}

static uint16_t *framebuffer_alloc(esp_lcd_panel_io_handle_t io)
{
    return spi_bus_dma_memory_alloc(LCD_HOST, FB_SIZE, 0);
}
#elif BOARD_LCD_I80
static esp_lcd_panel_io_handle_t panel_io_init(void)
{
    // Power the LCD (needed on battery) and hold the unused read strobe high
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LCD_POWER) | (1ULL << BOARD_PIN_RD),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(BOARD_PIN_LCD_POWER, 1);
    gpio_set_level(BOARD_PIN_RD, 1);

    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = BOARD_PIN_DC,
        .wr_gpio_num = BOARD_PIN_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = BOARD_PIN_DATA,
        .bus_width = 8,
        .max_transfer_bytes = FB_SIZE,
        .dma_burst_size = 64,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = BOARD_PIN_CS,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_flush_done,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io));
    return io;
}

static uint16_t *framebuffer_alloc(esp_lcd_panel_io_handle_t io)
{
    return esp_lcd_i80_alloc_draw_buffer(io, FB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}
#endif

void display_init(int brightness)
{
    backlight_init();

    s_flush_done = xSemaphoreCreateBinary();
    assert(s_flush_done);

    esp_lcd_panel_io_handle_t io = panel_io_init();

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE, // lets us write native uint16_t pixels
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true)); // IPS panel needs inversion
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));      // landscape
    display_set_flipped(false);

    s_fb = framebuffer_alloc(io);
    assert(s_fb);
    display_clear(COLOR_BLACK);
    display_flush();

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    display_set_brightness(brightness);
    ESP_LOGI(TAG, "ST7789 initialised (%s, %dx%d)", BOARD_NAME, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > DISPLAY_WIDTH ? DISPLAY_WIDTH : x + w;
    int y1 = y + h > DISPLAY_HEIGHT ? DISPLAY_HEIGHT : y + h;
    for (int row = y0; row < y1; row++) {
        uint16_t *p = &s_fb[row * DISPLAY_WIDTH];
        for (int col = x0; col < x1; col++) {
            p[col] = color;
        }
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - 0x20];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 8; row++) {
            if (glyph[col] & (1 << row)) {
                display_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

int display_text_width(const char *text, int scale)
{
    int n = strlen(text);
    // 5 px glyph + 1 px spacing, no trailing spacing
    return n ? (n * 6 - 1) * scale : 0;
}

void display_text(int x, int y, const char *text, int scale, uint16_t color)
{
    for (; *text; text++, x += 6 * scale) {
        draw_char(x, y, *text, scale, color);
    }
}

void display_text_centered(int y, const char *text, int scale, uint16_t color)
{
    display_text((DISPLAY_WIDTH - display_text_width(text, scale)) / 2, y, text, scale, color);
}

void display_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
#if BOARD_BL_PULSE_DIMMING
    // The AW9364 starts at full brightness when enabled, and each low pulse
    // on its enable pin drops it one of 16 levels, wrapping back to full.
    // Holding the pin low for over 2.5 ms turns it off.
    static int s_level; // 0 = off, BL_PULSE_STEPS = full
    int level = 0;
    if (percent > 0) {
        // Roughly even steps to the eye: 10% -> 1, 25% -> 2, 50% -> 6, 75% -> 10, 100% -> 16
        level = (int)(BL_PULSE_STEPS * powf(percent / 100.0f, 1.5f) + 0.5f);
        level = level < 1 ? 1 : level;
    }
    if (level == s_level) {
        return;
    }
    if (level == 0) {
        gpio_set_level(BOARD_PIN_BL, 0);
        esp_rom_delay_us(3000);
        s_level = 0;
        return;
    }
    if (s_level == 0) {
        gpio_set_level(BOARD_PIN_BL, 1);
        esp_rom_delay_us(30);
        s_level = BL_PULSE_STEPS;
    }
    int pulses = (s_level - level + BL_PULSE_STEPS) % BL_PULSE_STEPS;
    // Keep the pulses short: a low longer than 2.5 ms would switch it off
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < pulses; i++) {
        gpio_set_level(BOARD_PIN_BL, 0);
        esp_rom_delay_us(1);
        gpio_set_level(BOARD_PIN_BL, 1);
        esp_rom_delay_us(1);
    }
    portEXIT_CRITICAL(&mux);
    s_level = level;
#else
    // Square the level so equal steps look roughly equal to the eye
    uint32_t duty = BL_DUTY_MAX * percent * percent / 10000;
    ESP_ERROR_CHECK(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL));
#endif
}

void display_set_flipped(bool flipped)
{
    if (flipped) {
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, !BOARD_LCD_MIRROR_X, !BOARD_LCD_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_RAM_WIDTH - DISPLAY_WIDTH - BOARD_LCD_X_GAP,
                                              BOARD_LCD_RAM_HEIGHT - DISPLAY_HEIGHT - BOARD_LCD_Y_GAP));
    } else {
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP));
    }
}

uint16_t *display_framebuffer(void)
{
    return s_fb;
}

void display_flush(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, s_fb));
    // Wait for the DMA transfer so the caller can safely draw the next frame
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}
