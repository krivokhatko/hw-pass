/* ST7789 LCD (135x240) used in landscape as 240x135. StickS3: ST7789P3,
 * the backlight by PWM. StickC Plus: ST7789V2, the backlight on a
 * regulator of the AXP192. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "display.h"
#if CONFIG_HWPASS_AXP192
#include "axp192.h"
#else
#include "driver/ledc.h"
#endif

#define LCD_HOST        SPI3_HOST
#if CONFIG_HWPASS_BOARD_STICKCPLUS
#define LCD_MOSI        15
#define LCD_SCLK        13
#define LCD_DC          23
#define LCD_CS          5
#define LCD_RST         18
#else   /* StickS3 */
#define LCD_MOSI        39
#define LCD_SCLK        40
#define LCD_DC          45
#define LCD_CS          41
#define LCD_RST         21
#define LCD_BL          38
#endif
#define LCD_HZ          40000000
#define LCD_READ_HZ     10000000    /* Reads fail at the write clock. */
#define FRAME_BYTES     (DISPLAY_W * DISPLAY_H * 2)

#define BL_HZ           256         /* Same as M5GFX for this board. */
#define BL_MIN          16          /* Lowest duty that gives light. */
#define WAKE_MS         10          /* Wait after sleep out */

#define CMD_SLPIN       0x10
#define CMD_SLPOUT      0x11
#define CMD_INVON       0x21
#define CMD_DISPOFF     0x28
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_RASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_RDDPM       0x0A
#define CMD_MADCTL      0x36
#define CMD_COLMOD      0x3A

/* The controller RAM is 240x320. 240 - 135 = 105 is odd, thus the
 * row offset is 52 or 53. It depends on the mirror direction. */
static const struct {
    uint8_t madctl;
    uint8_t x_gap;
    uint8_t y_gap;
} s_rot[2] = {
    { 0x64, 40, 53 },   /* MV | MX | MH: M5GFX rotation 1 */
    { 0xB0, 40, 52 },   /* MV | MY | ML: M5GFX rotation 3 */
};

/* Power, gamma, and voltage settings from M5GFX Panel_ST7789. */
static const struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
} s_init[] = {
    { 0xB7, 1, { 0x35 } },
    { 0xBB, 1, { 0x28 } },
    { 0xC0, 1, { 0x0C } },
    { 0xC2, 2, { 0x01, 0xFF } },
    { 0xC3, 1, { 0x10 } },
    { 0xC4, 1, { 0x20 } },
    { 0xD0, 2, { 0xA4, 0xA1 } },
    { 0xB0, 2, { 0x00, 0xC0 } },
    { 0xE0, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                  0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17 } },
    { 0xE1, 14, { 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                  0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E } },
    { CMD_COLMOD, 1, { 0x55 } },    /* 16 bits for each pixel */
    { CMD_INVON, 0, { 0 } },
};

static const char *TAG = "display";
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static uint16_t *s_fb;
static uint8_t s_power_mode;
static int s_flip;
static bool s_ready;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;

    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t open_io(int hz, bool callback)
{
    esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = hz,
        .trans_queue_depth = 4,
        .on_color_trans_done = callback ? on_color_done : NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.sio_mode = 1,    /* MOSI is also the read line. */
    };
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &s_io);
}

static esp_err_t panel_setup(void)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "rst pin");
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, s_init[i].cmd, s_init[i].data, s_init[i].len),
                            TAG, "cmd 0x%02x", s_init[i].cmd);
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(130));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, CMD_MADCTL, &s_rot[0].madctl, 1), TAG, "madctl");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, CMD_DISPON, NULL, 0), TAG, "dispon");
    return esp_lcd_panel_io_rx_param(s_io, CMD_RDDPM, &s_power_mode, 1);
}

#if CONFIG_HWPASS_AXP192

/* The AXP192 has the backlight off from axp192_init. */
static esp_err_t backlight_setup(void)
{
    return ESP_OK;
}

static void backlight_set(uint8_t level)
{
    axp192_backlight(level);
}

#else   /* PWM */

static esp_err_t backlight_setup(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BL_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };

    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "bl timer");
    return ledc_channel_config(&channel);
}

static void backlight_set(uint8_t level)
{
    uint32_t duty = level ? BL_MIN + (255 - BL_MIN) * level / 255 : 0;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#endif  /* CONFIG_HWPASS_AXP192 */

esp_err_t display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_BYTES,
    };

    s_fb = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_fb && s_done, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(backlight_setup(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    /* Configure at the read clock, then reopen at the write clock. */
    ESP_RETURN_ON_ERROR(open_io(LCD_READ_HZ, false), TAG, "io");
    ESP_RETURN_ON_ERROR(panel_setup(), TAG, "panel");
    esp_lcd_panel_io_del(s_io);
    ESP_RETURN_ON_ERROR(open_io(LCD_HZ, true), TAG, "io");

    s_ready = true;
    display_flush();
    ESP_LOGI(TAG, "power mode 0x%02x", s_power_mode);
    return ESP_OK;
}

void display_set_flip(bool flip)
{
    s_flip = flip ? 1 : 0;
    if (s_ready) {
        esp_lcd_panel_io_tx_param(s_io, CMD_MADCTL, &s_rot[s_flip].madctl, 1);
    }
}

/* The panel sleeps while the backlight is off: its oscillator, charge
 * pumps, and scanning stop. The RAM keeps the picture, and writes to it
 * work. Sleep out needs WAKE_MS before the next command (datasheet: 5 ms).
 * The panel needs 120 ms between a sleep out and the next sleep in: the
 * idle time before a dark screen is much longer. */
static bool s_asleep;

static void panel_sleep(bool sleep)
{
    if (!s_ready || sleep == s_asleep) {
        return;
    }
    s_asleep = sleep;
    if (sleep) {
        esp_lcd_panel_io_tx_param(s_io, CMD_DISPOFF, NULL, 0);
        esp_lcd_panel_io_tx_param(s_io, CMD_SLPIN, NULL, 0);
    } else {
        esp_lcd_panel_io_tx_param(s_io, CMD_SLPOUT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(WAKE_MS));
        esp_lcd_panel_io_tx_param(s_io, CMD_DISPON, NULL, 0);
    }
}

void display_backlight(uint8_t level)
{
    if (level) {
        panel_sleep(false);
    }
    backlight_set(level);
    if (!level) {
        panel_sleep(true);
    }
}

uint8_t display_power_mode(void)
{
    return s_power_mode;
}

void display_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > DISPLAY_W) {
        w = DISPLAY_W - x;
    }
    if (y + h > DISPLAY_H) {
        h = DISPLAY_H - y;
    }
    for (int row = y; row < y + h; row++) {
        uint16_t *p = s_fb + row * DISPLAY_W + x;
        for (int i = 0; i < w; i++) {
            p[i] = color;
        }
    }
}

static void draw_glyph(int x, int y, char c, const font_t *f, uint16_t fg, uint16_t bg)
{
    int stride = (f->w + 7) / 8;
    const uint8_t *g;

    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    g = f->bits + (c - 0x20) * stride * f->h;
    for (int row = 0; row < f->h; row++, g += stride) {
        int py = y + row;
        if (py < 0 || py >= DISPLAY_H) {
            continue;
        }
        for (int col = 0; col < f->w; col++) {
            int px = x + col;
            if (px >= 0 && px < DISPLAY_W) {
                bool on = g[col / 8] & (0x80 >> (col % 8));
                s_fb[py * DISPLAY_W + px] = on ? fg : bg;
            }
        }
    }
}

int display_text(int x, int y, const char *s, const font_t *f, uint16_t fg, uint16_t bg)
{
    for (; *s && s_fb; s++, x += f->w) {
        draw_glyph(x, y, *s, f, fg, bg);
    }
    return x;
}

void display_flush(void)
{
    uint16_t x0 = s_rot[s_flip].x_gap, x1 = x0 + DISPLAY_W - 1;
    uint16_t y0 = s_rot[s_flip].y_gap, y1 = y0 + DISPLAY_H - 1;
    uint8_t caset[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t raset[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };

    if (!s_ready) {
        return;
    }
    esp_lcd_panel_io_tx_param(s_io, CMD_CASET, caset, sizeof(caset));
    esp_lcd_panel_io_tx_param(s_io, CMD_RASET, raset, sizeof(raset));
    esp_lcd_panel_io_tx_color(s_io, CMD_RAMWR, s_fb, FRAME_BYTES);
    xSemaphoreTake(s_done, portMAX_DELAY);
}
