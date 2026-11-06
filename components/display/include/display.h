#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Landscape frame size in pixels. */
#define DISPLAY_W   240
#define DISPLAY_H   135

/* RGB565 color, byte-swapped for the panel. Use the arguments 0..255. */
#define DISPLAY_RGB(r, g, b) ((uint16_t)( \
    (((r) & 0xF8) | ((g) >> 5)) | ((((g) & 0x1C) << 3 | ((b) >> 3)) << 8)))

/* Bitmap font with the glyphs 0x20..0x7E. */
typedef struct {
    uint8_t w;
    uint8_t h;
    const uint8_t *bits;
} font_t;

extern const font_t font_8x16;
extern const font_t font_12x24;

/* Start the LCD. The PM1 must supply the LCD power first. */
esp_err_t display_init(void);

/* Turn the picture 180 degrees. The change shows at the next flush. */
void display_set_flip(bool flip);

/* Set the backlight. 0 is off, 255 is full. At 0, the panel also sleeps:
 * it keeps the picture, and drawing works. A level above 0 wakes it, in
 * about 10 ms. Call from one task only. */
void display_backlight(uint8_t level);

/* Read the panel power mode (RDDPM) at start-up. 0x9C: on and awake. */
uint8_t display_power_mode(void);

/* Drawing operations change the frame buffer only. */
void display_fill(int x, int y, int w, int h, uint16_t color);

/* Draw text. Return the x position after the last glyph. */
int display_text(int x, int y, const char *s, const font_t *f, uint16_t fg, uint16_t bg);

/* Send the frame buffer to the LCD. */
void display_flush(void);

#ifdef __cplusplus
}
#endif
