/* Screen and buttons. The side button selects an entry. The front
 * button types the password of the entry. Both buttons together start
 * the Wi-Fi setup mode. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "qrcode.h"
#include "buttons.h"
#include "display.h"
#include "usbdev.h"
#include "vault.h"
#include "web.h"
#include "ui.h"

#define BRIGHTNESS      160
#define IDLE_OFF_MS     60000   /* Backlight off after this idle time. */
#define DONE_MS         600
#define ERROR_MS        1500
#define BOTH_MS         2000    /* Hold both buttons: Wi-Fi setup mode. */
#define HOLD_STEP_MS    100     /* Redraw interval of the hold progress bar */

/* Wi-Fi setup screen: QR code in a square at the left, text at the right. */
#define QR_BOX          DISPLAY_H
#define QR_VERSION_MAX  4
#define WEB_X           (QR_BOX + 4)

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
static volatile bool s_web_start;
static volatile bool s_web_stop;
static bool s_flip;
static int s_sel;
static uint32_t s_hold_ms;      /* Both buttons are down: progress bar */
static web_info_t s_web;

void ui_flip(void)
{
    s_flip_request = true;
}

void ui_dfu(void)
{
    s_dfu_request = true;
}

void ui_web(bool on)
{
    if (on) {
        s_web_start = true;
    } else {
        s_web_stop = true;
    }
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

static void flip(void)
{
    s_flip_request = false;
    s_flip = !s_flip;
    save_flip();
    display_set_flip(s_flip);
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
        text_center(LIST_Y + 44, "Hold both buttons: Wi-Fi", &font_8x16, C_DIM);
        text_center(LIST_Y + 62, "or USB serial: add <label>", &font_8x16, C_DIM);
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
    if (s_hold_ms > 0) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * s_hold_ms / BOTH_MS, 2, C_ACCENT);
        display_text(4, FOOTER_Y, "Hold for Wi-Fi setup", &font_8x16, C_ACCENT, C_BG);
    } else if (n > 0) {
        display_text(4, FOOTER_Y, "Side: next  Front: type", &font_8x16, C_DIM, C_BG);
    }
    display_flush();
}

/* Callback of esp_qrcode_generate. Dark modules on a light square with
 * a quiet zone of 2 modules. */
static void draw_qr(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    int scale = QR_BOX / (size + 4);
    int box = (size + 4) * scale, x0 = (QR_BOX - box) / 2;

    display_fill(x0, x0, box, box, C_TEXT);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y)) {
                display_fill(x0 + (x + 2) * scale, x0 + (y + 2) * scale, scale, scale, C_BG);
            }
        }
    }
}

/* Before a device connects: a QR code to join the network, the network
 * name and the password. After: a QR code of the page address. Android
 * sends browser requests over mobile data while the network has no
 * internet. Its sign-in window uses the Wi-Fi network. */
static void draw_web(void)
{
    esp_qrcode_config_t cfg = {
        .display_func = draw_qr,
        .max_qrcode_version = QR_VERSION_MAX,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
    };
    uint32_t left = web_left_s();
    bool connected = web_connected();
    char text[64];

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    if (connected) {
        snprintf(text, sizeof(text), "http://%s/", WEB_IP);
    } else {
        snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", s_web.ssid, s_web.password);
    }
    if (esp_qrcode_generate(&cfg, text) != ESP_OK) {
        display_text(4, 4, "QR error", &font_8x16, C_ERROR, C_BG);
    }

    snprintf(text, sizeof(text), "%lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
    display_text(WEB_X, 2, "Wi-Fi", &font_8x16, C_ACCENT, C_BG);
    display_text(DISPLAY_W - 2 - (int)strlen(text) * font_8x16.w, 2, text, &font_8x16, C_DIM, C_BG);
    if (connected) {
        display_text(WEB_X, 22, "Connected", &font_8x16, C_OK, C_BG);
        display_text(WEB_X, 42, "Phone: tap", &font_8x16, C_DIM, C_BG);
        display_text(WEB_X, 58, "Sign in", &font_8x16, C_TEXT, C_BG);
        display_text(WEB_X, 78, "PC: open", &font_8x16, C_DIM, C_BG);
        display_text(WEB_X, 94, WEB_IP, &font_8x16, C_TEXT, C_BG);
    } else {
        /* The password in two lines of 8 characters. */
        display_text(WEB_X, 22, s_web.ssid, &font_8x16, C_TEXT, C_BG);
        snprintf(text, sizeof(text), "%.8s", s_web.password);
        display_text(WEB_X, 44, text, &font_12x24, C_TEXT, C_BG);
        display_text(WEB_X, 68, s_web.password + 8, &font_12x24, C_TEXT, C_BG);
        display_text(WEB_X, 98, "Scan the QR", &font_8x16, C_DIM, C_BG);
    }
    display_text(WEB_X, 117, "Button: stop", &font_8x16, C_DIM, C_BG);
    display_flush();
    vault_zero(text, sizeof(text));
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

/* Wi-Fi setup mode. Return when a button push, the page, the command
 * line, or the timeout stops it. */
static void web_mode(void)
{
    bool armed = false;     /* Both buttons were up after the start. */
    bool connected = false;
    uint32_t left = 0;
    esp_err_t err;

    display_backlight(BRIGHTNESS);
    show_message("Wi-Fi setup", "Starting", C_ACCENT);
    err = web_start(&s_web);
    if (err != ESP_OK) {
        vault_zero(&s_web, sizeof(s_web));
        show_message("Wi-Fi error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err), C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    draw_web();
    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();
        bool dirty = false;

        armed |= both == 0;
        s_web_start = false;    /* The mode is on. */
        if (side != PRESS_NONE || front != PRESS_NONE || (armed && both > 0) ||
            s_web_stop || s_dfu_request || web_left_s() == 0) {
            break;
        }
        if (s_flip_request) {
            flip();
            dirty = true;
        }
        if (left != web_left_s() || connected != web_connected()) {
            left = web_left_s();
            connected = web_connected();
            dirty = true;
        }
        if (dirty) {
            draw_web();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
    s_web_stop = false;
    show_message("Wi-Fi setup", "Stopping", C_ACCENT);
    web_stop();
    vault_zero(&s_web, sizeof(s_web));
    show_message("Wi-Fi off", NULL, C_OK);
    vTaskDelay(pdMS_TO_TICKS(DONE_MS));
}

void ui_run(void)
{
    uint32_t generation = vault_generation();
    bool usb = usbdev_ready();
    bool screen_on = true;
    bool both_used = false;     /* The current push of both buttons did its action. */
    uint32_t idle_ms = 0;

    /* The QR code library logs the text: it contains the Wi-Fi password. */
    esp_log_level_set("QRCODE", ESP_LOG_WARN);
    buttons_init();
    load_flip();
    display_set_flip(s_flip);
    draw_list();
    display_backlight(BRIGHTNESS);

    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();
        uint32_t hold = 0;
        bool dirty = false;
        int n;

        if (s_dfu_request) {
            show_message("Download mode", "Flash with idf.py", C_ACCENT);
            display_backlight(BRIGHTNESS);
            usbdev_dfu();
        }
        if (s_flip_request) {
            flip();
            dirty = true;
        }
        if (both == 0) {
            both_used = false;
        } else if (!both_used) {
            hold = both;
        }
        s_web_stop = false;     /* The mode is off. */
        if (s_web_start || hold >= BOTH_MS) {
            s_web_start = false;
            s_hold_ms = hold = 0;
            web_mode();
            /* The buttons were not sampled during the start, the stop and
             * the messages. A push in this time must not type. Both buttons
             * can still be down: this push is used. */
            buttons_consume();
            both_used = true;
            side = front = PRESS_NONE;
            screen_on = true;
            idle_ms = 0;
            dirty = true;
        }
        if (hold / HOLD_STEP_MS != s_hold_ms / HOLD_STEP_MS) {
            s_hold_ms = hold;
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

        if (side != PRESS_NONE || front != PRESS_NONE || both > 0) {
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
