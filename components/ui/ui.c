/* Screen and buttons. The side button selects an entry. The front
 * button types the password of the entry. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "buttons.h"
#include "display.h"
#include "usbdev.h"
#include "vault.h"
#include "ui.h"

#define BRIGHTNESS      160
#define IDLE_OFF_MS     60000   /* Backlight off after this idle time. */
#define DONE_MS         600
#define ERROR_MS        1500

/* Layout: header, list of ROWS entries, footer. */
#define HEADER_Y        1
#define LIST_Y          21
#define ROW_H           24
#define ROWS            4
#define FOOTER_Y        (DISPLAY_H - 16)

#define C_BG            DISPLAY_RGB(0, 0, 0)
#define C_TEXT          DISPLAY_RGB(255, 255, 255)
#define C_DIM           DISPLAY_RGB(130, 130, 130)
#define C_LINE          DISPLAY_RGB(60, 60, 60)
#define C_ACCENT        DISPLAY_RGB(255, 176, 0)
#define C_OK            DISPLAY_RGB(0, 200, 80)
#define C_ERROR         DISPLAY_RGB(230, 40, 40)

static const char *TAG = "ui";
static volatile bool s_flip_request;
static volatile bool s_dfu_request;
static bool s_flip;
static int s_sel;

void ui_flip(void)
{
    s_flip_request = true;
}

void ui_dfu(void)
{
    s_dfu_request = true;
}

static void load_flip(void)
{
    nvs_handle_t h;
    uint8_t v = 0;

    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "flip", &v);
        nvs_close(h);
    }
    s_flip = v != 0;
}

static void save_flip(void)
{
    nvs_handle_t h;

    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, "flip", s_flip) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "cannot save the screen setting");
        }
        nvs_close(h);
    }
}

/* ---- Drawing ---- */

static void text_center(int y, const char *s, const font_t *f, uint16_t fg)
{
    display_text((DISPLAY_W - (int)strlen(s) * f->w) / 2, y, s, f, fg, C_BG);
}

static void draw_frame(void)
{
    char count[24];
    int n = vault_count();
    int x = DISPLAY_W - 4 - 3 * font_8x16.w;

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "hw-pass", &font_8x16, C_DIM, C_BG);
    if (n > 0) {
        snprintf(count, sizeof(count), "%d/%d", s_sel + 1, n);
        display_text(x - (2 + (int)strlen(count)) * font_8x16.w, HEADER_Y, count, &font_8x16, C_TEXT, C_BG);
    }
    display_text(x, HEADER_Y, "USB", &font_8x16, usbdev_ready() ? C_OK : C_ERROR, C_BG);
    display_fill(0, LIST_Y - 3, DISPLAY_W, 1, C_LINE);
    display_fill(0, FOOTER_Y - 2, DISPLAY_W, 1, C_LINE);
}

static void draw_list(void)
{
    char label[VAULT_LABEL_MAX + 1];
    int n = vault_count();
    int first = s_sel - 1;  /* One entry of context above the selection. */

    draw_frame();
    if (n == 0) {
        text_center(LIST_Y + 8, "No entries", &font_12x24, C_TEXT);
        text_center(LIST_Y + 44, "Open the USB serial port", &font_8x16, C_DIM);
        text_center(LIST_Y + 62, "and type: add <label>", &font_8x16, C_DIM);
    }
    if (first > n - ROWS) {
        first = n - ROWS;
    }
    if (first < 0) {
        first = 0;
    }
    for (int r = 0; r < ROWS && first + r < n; r++) {
        int i = first + r, y = LIST_Y + r * ROW_H;
        uint16_t fg = i == s_sel ? C_BG : C_TEXT, bg = i == s_sel ? C_ACCENT : C_BG;

        display_fill(0, y, DISPLAY_W, ROW_H, bg);
        if (vault_label(i, label, sizeof(label)) == ESP_OK) {
            display_text(6, y, label, &font_12x24, fg, bg);
        }
    }
    if (n > 0) {
        display_text(4, FOOTER_Y, "Side: next  Front: type", &font_8x16, C_DIM, C_BG);
    }
    display_flush();
}

static void show_message(const char *title, const char *detail, uint16_t color)
{
    draw_frame();
    text_center(LIST_Y + 14, title, &font_12x24, color);
    if (detail) {
        text_center(LIST_Y + 50, detail, &font_12x24, C_TEXT);
    }
    display_flush();
}

/* ---- Actions ---- */

static void type_selected(bool enter)
{
    char label[VAULT_LABEL_MAX + 1], secret[VAULT_SECRET_MAX + 1];
    esp_err_t err;

    if (vault_label(s_sel, label, sizeof(label)) != ESP_OK) {
        return;
    }
    if (!usbdev_ready()) {
        show_message("No USB host", "Connect to a PC", C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    show_message(enter ? "Typing + Enter" : "Typing", label, C_ACCENT);
    err = vault_secret(s_sel, secret, sizeof(secret));
    if (err == ESP_OK) {
        err = usbdev_type(secret, enter);
    }
    vault_zero(secret, sizeof(secret));
    if (err == ESP_OK) {
        show_message("Done", label, C_OK);
        vTaskDelay(pdMS_TO_TICKS(DONE_MS));
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        show_message("Caps Lock error", "Turn it off", C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
    } else {
        show_message("Typing failed", label, C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
    }
}

void ui_run(void)
{
    uint32_t generation = vault_generation();
    bool usb = usbdev_ready();
    bool screen_on = true;
    uint32_t idle_ms = 0;

    buttons_init();
    load_flip();
    display_set_flip(s_flip);
    draw_list();
    display_backlight(BRIGHTNESS);

    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        bool dirty = false;
        int n;

        if (s_dfu_request) {
            show_message("Download mode", "Flash with idf.py", C_ACCENT);
            display_backlight(BRIGHTNESS);
            usbdev_dfu();
        }
        if (s_flip_request) {
            s_flip_request = false;
            s_flip = !s_flip;
            save_flip();
            display_set_flip(s_flip);
            dirty = true;
        }
        if (generation != vault_generation()) {
            generation = vault_generation();
            idle_ms = 0;
            dirty = true;
        }
        /* Read the count after the generation. The vault changes the
         * count first, thus this count is not older than the generation. */
        n = vault_count();
        if (s_sel >= n) {
            s_sel = n > 0 ? n - 1 : 0;
        }
        if (usb != usbdev_ready()) {
            usb = !usb;
            dirty = true;
        }

        if (side != PRESS_NONE || front != PRESS_NONE) {
            idle_ms = 0;
            /* When the screen is off, a push only turns it on. */
            if (!screen_on) {
                side = front = PRESS_NONE;
                dirty = true;
            }
        }
        if (n > 0 && side != PRESS_NONE) {
            s_sel = (s_sel + (side == PRESS_SHORT ? 1 : n - 1)) % n;
            dirty = true;
        }
        if (n > 0 && front != PRESS_NONE) {
            type_selected(front == PRESS_LONG);
            dirty = true;
        }

        if (dirty) {
            draw_list();
        }
        if (idle_ms == 0 && !screen_on) {
            screen_on = true;
            display_backlight(BRIGHTNESS);
        }
        idle_ms += BUTTONS_POLL_MS;
        if (screen_on && idle_ms >= IDLE_OFF_MS) {
            screen_on = false;
            display_backlight(0);
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
}
