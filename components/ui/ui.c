/* Screen and buttons. The side button selects an entry. The front
 * button types the password of the entry. Both buttons together start
 * the Wi-Fi setup mode or the pairing of a keypad. A keypad key types
 * the entry that has this key. The keypad keys - and + select an entry,
 * and * turns the mouse mover on or off. */
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
#include "keypad.h"
#include "usbdev.h"
#include "vault.h"
#include "web.h"
#include "ui.h"

#define BRIGHTNESS      160
#define IDLE_OFF_MS     60000   /* Backlight off after this idle time. */
#define DONE_MS         600
#define ERROR_MS        1500
#define INFO_MS         1500    /* Message after a pairing or a key change */
#define BOTH_MS         2000    /* Hold both buttons, then release: Wi-Fi setup mode. */
#define PAIR_MS         5000    /* Hold both buttons longer: pair a keypad. */
#define BIND_MS         2000    /* Hold a keypad key: it gets the selected entry. */
#define HINT_MS         300     /* Keypad hold time before the progress bar shows */
#define HOLD_STEP_MS    100     /* Redraw interval of the hold progress bar */

/* Keypad keys that go to the USB host: HID usage IDs */
#define KEY_ENTER       0x28
#define KEY_BACKSPACE   0x2A
#define KEY_PAD_ENTER   0x58
/* Keypad keys with a function in the UI */
#define KEY_PAD_STAR    0x55    /* Mouse mover on or off */
#define KEY_PAD_MINUS   0x56    /* Select the previous entry. */
#define KEY_PAD_PLUS    0x57    /* Select the next entry. */

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
#define C_BT            DISPLAY_RGB(60, 140, 255)

static const char *TAG = "ui";
static volatile bool s_flip_request;
static volatile bool s_dfu_request;
static volatile bool s_web_start;
static volatile bool s_web_stop;
static bool s_flip;
static int s_sel;
static uint32_t s_hold_ms;      /* Both buttons are down: progress bar */
static uint8_t s_kp_key;        /* The keypad key that is down, or 0 */
static uint32_t s_kp_ms;        /* Time of its push */
static uint32_t s_kp_held;      /* Hold time of the progress bar */
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

/* Settings: "flip" (screen) and "mouse" (mouse mover). Default 0. */
static uint8_t load_setting(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;

    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void save_setting(const char *key, uint8_t v)
{
    nvs_handle_t h;

    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, key, v) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "cannot save the setting %s", key);
        }
        nvs_close(h);
    }
}

void ui_mouse(bool on)
{
    usbdev_mouse_mover(on);
    save_setting("mouse", on);
}

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

static void flip(void)
{
    s_flip_request = false;
    s_flip = !s_flip;
    save_setting("flip", s_flip);
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
    keypad_state_t kp = keypad_state();

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "hw-pass", &font_8x16, C_DIM, C_BG);
    display_text(x, HEADER_Y, "USB", &font_8x16, usbdev_ready() ? C_OK : C_ERROR, C_BG);
    if (kp != KEYPAD_NONE) {
        x -= 3 * font_8x16.w;
        display_text(x, HEADER_Y, "BT", &font_8x16, kp == KEYPAD_CONNECTED ? C_OK : C_DIM, C_BG);
    }
    if (usbdev_mouse_mover_on()) {
        x -= 6 * font_8x16.w;
        display_text(x, HEADER_Y, "Mouse", &font_8x16, usbdev_ready() ? C_OK : C_DIM, C_BG);
    }
    if (n > 0) {
        snprintf(count, sizeof(count), "%d/%d", s_sel + 1, n);
        display_text(x - (2 + (int)strlen(count)) * font_8x16.w, HEADER_Y, count, &font_8x16, C_TEXT, C_BG);
    }
    display_fill(0, LIST_Y - 3, DISPLAY_W, 1, C_LINE);
    display_fill(0, FOOTER_Y - 2, DISPLAY_W, 1, C_LINE);
}

/* The footer during a hold of both buttons: 2 s, then 3 s more. */
static void draw_both_hold(void)
{
    if (s_hold_ms < BOTH_MS) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * s_hold_ms / BOTH_MS, 2, C_ACCENT);
        display_text(4, FOOTER_Y, "Hold for Wi-Fi setup", &font_8x16, C_ACCENT, C_BG);
    } else if (s_hold_ms < PAIR_MS) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * (s_hold_ms - BOTH_MS) / (PAIR_MS - BOTH_MS), 2, C_BT);
        display_text(4, FOOTER_Y, "Release: Wi-Fi. Hold: keypad", &font_8x16, C_ACCENT, C_BG);
    } else {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W, 2, C_BT);
        display_text(4, FOOTER_Y, "Release: pair keypad", &font_8x16, C_BT, C_BG);
    }
}

static void draw_list(void)
{
    char label[VAULT_LABEL_MAX + 1], name[KEYPAD_KEY_NAME], text[32];
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
        uint8_t key = vault_key(i);
        int right = DISPLAY_W;  /* The label ends before the key name. */

        display_fill(0, y, DISPLAY_W, ROW_H, bg);
        if (key != 0) {
            keypad_key_name(key, name);
            right = DISPLAY_W - 4 - (int)strlen(name) * font_8x16.w;
            display_text(right, y + 4, name, &font_8x16, i == s_sel ? C_BG : C_DIM, bg);
        }
        if (vault_label(i, label, sizeof(label)) == ESP_OK) {
            int max = (right - 6 - 4) / font_12x24.w;

            if ((int)strlen(label) > max) {
                label[max] = '\0';
            }
            display_text(6, y, label, &font_12x24, fg, bg);
        }
    }
    if (s_hold_ms > 0) {
        draw_both_hold();
    } else if (s_kp_held >= HINT_MS) {
        keypad_key_name(s_kp_key, name);
        snprintf(text, sizeof(text), s_kp_held >= BIND_MS ? "Release: bind key %s" : "Hold to bind key %s",
                 name);
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * s_kp_held / BIND_MS, 2, C_BT);
        display_text(4, FOOTER_Y, text, &font_8x16, C_BT, C_BG);
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

/* Select the next (1) or the previous (-1) entry. After the last entry
 * comes the first. */
static void select_step(int step)
{
    int n = vault_count();

    if (n > 0) {
        s_sel = (s_sel + step + n) % n;
    }
}

static void type_entry(int i, bool enter)
{
    char label[VAULT_LABEL_MAX + 1], secret[VAULT_SECRET_MAX + 1];
    esp_err_t err;

    if (vault_label(i, label, sizeof(label)) != ESP_OK) {
        return;
    }
    if (!usbdev_ready()) {
        show_message("No USB host", "Connect to a PC", C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    show_message(enter ? "Typing + Enter" : "Typing", label, C_ACCENT);
    err = vault_secret(i, secret, sizeof(secret));
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

/* A tap of a keypad key types the entry that has this key. */
static void type_key(uint8_t key)
{
    char name[KEYPAD_KEY_NAME], title[32];
    int i = vault_find_key(key);

    if (i >= 0) {
        type_entry(i, false);
        return;
    }
    keypad_key_name(key, name);
    snprintf(title, sizeof(title), "Key %s: no entry", name);
    show_message(title, "Hold 2 s: bind", C_ACCENT);
    vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
}

/* A hold of a keypad key gives it the selected entry. If the entry has
 * this key already, the key loses the entry. */
static void bind_key(uint8_t key)
{
    char label[VAULT_LABEL_MAX + 1], name[KEYPAD_KEY_NAME], title[32];
    bool remove;
    esp_err_t err;

    if (vault_label(s_sel, label, sizeof(label)) != ESP_OK) {
        show_message("No entries", NULL, C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    keypad_key_name(key, name);
    remove = vault_key(s_sel) == key;
    err = vault_set_key(s_sel, remove ? 0 : key);
    if (err != ESP_OK) {
        show_message("Write error", esp_err_to_name(err), C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    snprintf(title, sizeof(title), remove ? "Key %s removed" : "Key %s types", name);
    show_message(title, label, C_OK);
    vTaskDelay(pdMS_TO_TICKS(INFO_MS));
}

/* A tap of * turns the mouse mover on or off. It has no entry. */
static void toggle_mouse(void)
{
    bool on = !usbdev_mouse_mover_on();

    ui_mouse(on);
    show_message(on ? "Mouse mover on" : "Mouse mover off", on ? "PC stays awake" : "PC can sleep", C_OK);
    vTaskDelay(pdMS_TO_TICKS(INFO_MS));
}

/* Enter and Backspace go to the USB host as they are: after a password,
 * or to correct text. They have no entry. */
static bool passthru(uint8_t key)
{
    return key == KEY_ENTER || key == KEY_BACKSPACE || key == KEY_PAD_ENTER;
}

/* Read the keypad keys. A tap types, a hold binds. The times come from
 * the events: a slow loop does not change a tap into a hold. Only the
 * release binds: after a lost link, the cancel comes some seconds late,
 * and a tap must not become a hold. Before an action, the host gets the
 * release of all keys: else it repeats a held key while the UI does not
 * read the keypad. A hold of * does nothing: the FBK21C uses a hold of
 * Enter and * for its pairing mode. */
static bool poll_keypad(void)
{
    keypad_event_t ev;
    uint32_t held;
    bool dirty = false;

    while (keypad_event(&ev)) {
        dirty = true;
        if (ev.key == 0) {
            usbdev_key(0, false);
            s_kp_key = 0;
            s_kp_held = 0;
        } else if (passthru(ev.key)) {
            usbdev_key(ev.key, ev.down);
        } else if (ev.key == KEY_PAD_MINUS || ev.key == KEY_PAD_PLUS) {
            if (ev.down) {
                select_step(ev.key == KEY_PAD_PLUS ? 1 : -1);
            }
        } else if (ev.down && s_kp_key == 0) {
            s_kp_key = ev.key;
            s_kp_ms = ev.ms;
        } else if (!ev.down && ev.key == s_kp_key) {
            s_kp_key = 0;
            s_kp_held = 0;
            usbdev_key(0, false);
            display_backlight(BRIGHTNESS);
            if (ev.key == KEY_PAD_STAR) {
                if (ev.ms - s_kp_ms < BIND_MS) {
                    toggle_mouse();
                }
            } else if (ev.ms - s_kp_ms >= BIND_MS) {
                bind_key(ev.key);
            } else {
                type_key(ev.key);
            }
            break;  /* Read the next events in the next cycle. */
        }
    }
    /* * cannot bind: no progress bar. */
    held = s_kp_key != 0 && s_kp_key != KEY_PAD_STAR ? now_ms() - s_kp_ms : 0;
    if (held > BIND_MS) {
        held = BIND_MS;     /* The bar is full: no more redraws. */
    }
    if (held / HOLD_STEP_MS != s_kp_held / HOLD_STEP_MS) {
        s_kp_held = held;
        dirty = true;
    }
    return dirty;
}

static void draw_pair(const keypad_pair_t *p)
{
    char text[16];

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "Pair keypad", &font_8x16, C_BT, C_BG);
    switch (p->step) {
    case KEYPAD_PAIR_SEARCHING:
        text_center(LIST_Y + 4, "Searching", &font_12x24, C_ACCENT);
        text_center(LIST_Y + 40, "Turn on pairing mode", &font_8x16, C_TEXT);
        text_center(LIST_Y + 58, "on the keypad", &font_8x16, C_TEXT);
        break;
    case KEYPAD_PAIR_CONNECTING:
        text_center(LIST_Y + 4, "Connecting", &font_12x24, C_ACCENT);
        text_center(LIST_Y + 40, p->name, &font_8x16, C_TEXT);
        break;
    case KEYPAD_PAIR_PASSKEY:
        snprintf(text, sizeof(text), "%06lu", (unsigned long)p->passkey);
        text_center(LIST_Y, "Type on the keypad:", &font_8x16, C_TEXT);
        text_center(LIST_Y + 22, text, &font_12x24, C_ACCENT);
        text_center(LIST_Y + 54, "then Enter", &font_8x16, C_TEXT);
        break;
    case KEYPAD_PAIR_DONE:
        text_center(LIST_Y + 4, "Keypad paired", &font_12x24, C_OK);
        text_center(LIST_Y + 40, p->name, &font_8x16, C_TEXT);
        break;
    case KEYPAD_PAIR_FAILED:
        text_center(LIST_Y + 4, "Pairing failed", &font_12x24, C_ERROR);
        text_center(LIST_Y + 40, p->error ? p->error : "", &font_8x16, C_TEXT);
        break;
    }
    if (p->step != KEYPAD_PAIR_DONE && p->step != KEYPAD_PAIR_FAILED) {
        display_text(4, FOOTER_Y, "Button: stop", &font_8x16, C_DIM, C_BG);
    }
    display_flush();
}

/* Pairing of a keypad. Return when it is done, when it fails, or when a
 * button stops it. */
static void pair_mode(void)
{
    keypad_pair_t pair, shown = { 0 };
    bool armed = false;     /* Both buttons were up after the start. */
    bool first = true;
    esp_err_t err;

    display_backlight(BRIGHTNESS);
    err = keypad_pair_start();
    if (err != ESP_OK) {
        show_message("Bluetooth error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err), C_ERROR);
        vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
        return;
    }
    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();

        armed |= both == 0;
        keypad_pair_status(&pair);
        if (first || s_flip_request || pair.step != shown.step || pair.passkey != shown.passkey ||
            strcmp(pair.name, shown.name) != 0) {
            if (s_flip_request) {
                flip();
            }
            draw_pair(&pair);
            shown = pair;
            first = false;
        }
        if (pair.step == KEYPAD_PAIR_DONE || pair.step == KEYPAD_PAIR_FAILED) {
            vTaskDelay(pdMS_TO_TICKS(pair.step == KEYPAD_PAIR_DONE ? INFO_MS : ERROR_MS));
            break;
        }
        if (side != PRESS_NONE || front != PRESS_NONE || (armed && both > 0) || s_dfu_request) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
    keypad_pair_stop();
}

/* Wi-Fi setup mode. Return when a button push, the page, the command
 * line, or the timeout stops it. buttons: the buttons started it. */
static void web_mode(bool buttons)
{
    bool armed = false;     /* Both buttons were up after the start. */
    bool connected = false;
    uint32_t left = 0;
    esp_err_t err;

    display_backlight(BRIGHTNESS);
    show_message("Wi-Fi setup", "Starting", C_ACCENT);
    err = web_start(&s_web, buttons);
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
    keypad_state_t kp = keypad_state();
    bool mouse;
    bool screen_on = true;
    bool both_used = false;     /* The current push of both buttons did its action. */
    uint32_t idle_ms = 0;

    /* The QR code library logs the text: it contains the Wi-Fi password. */
    esp_log_level_set("QRCODE", ESP_LOG_WARN);
    buttons_init();
    s_flip = load_setting("flip") != 0;
    display_set_flip(s_flip);
    usbdev_mouse_mover(load_setting("mouse") != 0);
    mouse = usbdev_mouse_mover_on();
    draw_list();
    display_backlight(BRIGHTNESS);

    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();
        uint32_t hold = 0;
        bool web = false, pair = false;
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
        /* Hold both buttons, then release them. The last hold time on the
         * screen selects the mode. */
        if (both == 0) {
            web = !both_used && s_hold_ms >= BOTH_MS && s_hold_ms < PAIR_MS;
            pair = !both_used && s_hold_ms >= PAIR_MS;
            both_used = false;
        } else if (!both_used) {
            hold = both;
        }
        s_web_stop = false;     /* The mode is off. */
        if (s_web_start || web || pair) {
            /* A request from the command line counts first: the USB host
             * gets the Wi-Fi password. */
            bool cli = s_web_start;

            s_web_start = false;
            s_hold_ms = hold = 0;
            usbdev_key(0, false);
            if (cli || web) {
                web_mode(!cli);
            } else {
                pair_mode();
            }
            /* The buttons and the keypad were not read during the mode and
             * the messages. A push in this time must not type. Both
             * buttons can still be down: this push is used. */
            buttons_consume();
            keypad_flush();
            s_kp_key = 0;
            s_kp_held = 0;
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
        if (kp != keypad_state()) {
            kp = keypad_state();
            dirty = true;
        }
        if (mouse != usbdev_mouse_mover_on()) {
            mouse = !mouse;
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
            select_step(side == PRESS_SHORT ? 1 : -1);
            dirty = true;
        }
        if (n > 0 && front != PRESS_NONE) {
            type_entry(s_sel, front == PRESS_LONG);
            dirty = true;
        }
        /* A keypad key works also when the screen is off. */
        if (poll_keypad()) {
            idle_ms = 0;
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
