/* Screen, buttons, and sounds. The side button selects an entry. The
 * front button types the password of the entry. Both buttons together
 * start the Wi-Fi setup mode or the pairing of a keypad. A keypad key
 * types the entry that has this key. The keypad keys - and + select an
 * entry, and * turns the mouse mover on or off. A sound tells the result
 * of each action: the user does not have to look at the screen.
 *
 * With a PIN, the device starts locked and locks again when the USB
 * connection stops. The PIN screen shows a row of digits: the side
 * button moves, the front button picks. The keypad digits, Enter, and
 * Backspace also work. Nothing else works while locked.
 *
 * The screen turns with the device: the tilt sensor tells which edge is
 * up. When the device lies flat, the screen keeps its orientation, and
 * the "rotate" command turns it. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "qrcode.h"
#include "buttons.h"
#include "clock.h"
#include "display.h"
#include "imu.h"
#include "keypad.h"
#include "mover.h"
#include "ntp.h"
#include "sound.h"
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
#define BATTERY_LOW     20      /* Keypad battery level in %: red at this level and below */
#define TILT_MS         100     /* Read interval of the tilt sensor */
#define TILT_STABLE     5       /* Reads in a row with the same result before the screen turns: 0.5 s */

/* Keypad keys that go to the USB host: HID usage IDs */
#define KEY_ENTER       0x28
#define KEY_BACKSPACE   0x2A
#define KEY_PAD_ENTER   0x58
/* Keypad keys with a function in the UI */
#define KEY_PAD_STAR    0x55    /* Mouse mover on or off */
#define KEY_PAD_MINUS   0x56    /* Select the previous entry. */
#define KEY_PAD_PLUS    0x57    /* Select the next entry. */
/* Digits for the PIN: the keypad block and the top row */
#define KEY_1           0x1E
#define KEY_9           0x26
#define KEY_0           0x27
#define KEY_PAD_1       0x59
#define KEY_PAD_9       0x61
#define KEY_PAD_0       0x62

/* PIN screen: the items of the row. 0 to 9, then delete and OK. */
#define PIN_ITEMS       12
#define PIN_BACK        10
#define PIN_OK          11
#define PIN_GAP         6       /* Pixels between the items */

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
static imu_up_t s_up;           /* The stable result of the tilt sensor */
static imu_up_t s_up_new;       /* The last result, and how many reads in a row gave it */
static int s_up_count;
static uint32_t s_up_ms;        /* Time of the last read */
static int s_sel;
static uint32_t s_hold_ms;      /* Both buttons are down: progress bar */
static uint8_t s_kp_key;        /* The keypad key that is down, or 0 */
static uint32_t s_kp_ms;        /* Time of its push */
static uint32_t s_kp_held;      /* Hold time of the progress bar */
static web_info_t s_web;
static char s_pin[VAULT_PIN_MAX + 1];   /* The digits so far */
static int s_pin_len;
static int s_pin_cur;           /* The item under the cursor */

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

/* Settings: "flip" (screen) and "mute" (sounds). Default 0. */
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

/* When the sounds go on, a sound tells it. */
void ui_sound(bool on)
{
    sound_enable(on);
    save_setting("mute", !on);
    sound_play(SOUND_ON);
}

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

/* Minute of the day, or -1 when the time is not set. */
static int clock_minute(void)
{
    struct tm t;

    return clock_get(&t) ? t.tm_hour * 60 + t.tm_min : -1;
}

static void set_flip(bool flip)
{
    if (flip != s_flip) {
        s_flip = flip;
        save_setting("flip", s_flip);
    }
    display_set_flip(s_flip);
}

/* The command line asks for a turn, or the device was turned. The tilt
 * sensor reads each TILT_MS, and a new result must hold for TILT_STABLE
 * reads. Only a change of the result turns the screen: a turn by the
 * command line holds until the device is turned. When the device lies
 * flat, the sensor gives no edge. Return true when the screen turned. */
static bool poll_flip(void)
{
    uint32_t now = now_ms();
    bool turned = false;
    imu_up_t up;

    if (s_flip_request) {
        s_flip_request = false;
        set_flip(!s_flip);
        turned = true;
    }
    if (now - s_up_ms < TILT_MS) {
        return turned;
    }
    s_up_ms = now;
    up = imu_up();
    if (up != s_up_new) {
        s_up_new = up;
        s_up_count = 1;
    } else if (s_up_count < TILT_STABLE) {
        s_up_count++;
    }
    if (s_up_count == TILT_STABLE && up != s_up) {
        s_up = up;
        if (up != IMU_FLAT && s_flip != (up == IMU_BOTTOM_UP)) {
            set_flip(up == IMU_BOTTOM_UP);
            turned = true;
        }
    }
    return turned;
}

/* ---- Drawing ---- */

static void text_center(int y, const char *s, const font_t *f, uint16_t fg)
{
    display_text((DISPLAY_W - (int)strlen(s) * f->w) / 2, y, s, f, fg, C_BG);
}

/* The header items from the right: USB, keypad and its battery level,
 * mouse mover, position. With all items, the title just fits:
 * "hw-pass 32/32  MM BT 100% USB". */
static void draw_frame(void)
{
    char text[24];
    int n = vault_count();
    int x = DISPLAY_W - 4 - 3 * font_8x16.w;
    keypad_state_t kp = keypad_state();
    int battery = keypad_battery();

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "hw-pass", &font_8x16, C_DIM, C_BG);
    display_text(x, HEADER_Y, "USB", &font_8x16, usbdev_ready() ? C_OK : C_ERROR, C_BG);
    if (kp != KEYPAD_NONE) {
        uint16_t color = kp == KEYPAD_CONNECTED ? C_OK : C_DIM;

        /* Gray: the level at the last connection. */
        if (battery >= 0) {
            snprintf(text, sizeof(text), "%d%%", battery);
            x -= (1 + (int)strlen(text)) * font_8x16.w;
            display_text(x, HEADER_Y, text, &font_8x16, battery <= BATTERY_LOW ? C_ERROR : color, C_BG);
        }
        x -= 3 * font_8x16.w;
        display_text(x, HEADER_Y, "BT", &font_8x16, color, C_BG);
    }
    if (mover_on()) {
        x -= 3 * font_8x16.w;
        display_text(x, HEADER_Y, "MM", &font_8x16, usbdev_ready() ? C_OK : C_DIM, C_BG);
    }
    if (n > 0) {
        snprintf(text, sizeof(text), "%d/%d", s_sel + 1, n);
        display_text(x - (2 + (int)strlen(text)) * font_8x16.w, HEADER_Y, text, &font_8x16, C_TEXT, C_BG);
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

/* The time at the right of the footer. Without the time, the schedule of
 * the mouse mover cannot run: then "--:--" asks for the time. */
static void draw_clock(void)
{
    mover_sched_t sched;
    struct tm t;
    char text[8];
    int x = DISPLAY_W - 4 - 5 * font_8x16.w;

    if (clock_get(&t)) {
        snprintf(text, sizeof(text), "%02d:%02d", t.tm_hour, t.tm_min);
        display_text(x, FOOTER_Y, text, &font_8x16, C_DIM, C_BG);
        return;
    }
    mover_get_sched(&sched);
    if (sched.enabled) {
        display_text(x, FOOTER_Y, "--:--", &font_8x16, C_ACCENT, C_BG);
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
    } else {
        if (n > 0) {
            display_text(4, FOOTER_Y, "Side: next  Front: type", &font_8x16, C_DIM, C_BG);
        }
        draw_clock();
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

static void show_error(const char *title, const char *detail)
{
    show_message(title, detail, C_ERROR);
    sound_play(SOUND_ERROR);
    vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
}

/* The PIN screen: a line of state, the digits so far as stars, and the
 * row of items with the cursor. */
static void draw_pin(void)
{
    char text[32];      /* Also the stars: 2 * VAULT_PIN_MAX + 1 */
    int wait = vault_wait_s();
    int x = (DISPLAY_W - (10 * (font_12x24.w + PIN_GAP) + font_12x24.w + PIN_GAP + 2 * font_12x24.w)) / 2;
    int y = LIST_Y + 56;

    draw_frame();
    if (wait > 0) {
        snprintf(text, sizeof(text), "Wrong PIN. Wait %d s", wait);
        text_center(LIST_Y + 2, text, &font_8x16, C_ERROR);
    } else if (vault_fails() > 0) {
        text_center(LIST_Y + 2, "Wrong PIN. Try again", &font_8x16, C_ERROR);
    } else {
        text_center(LIST_Y + 2, "Enter the PIN", &font_8x16, C_TEXT);
    }
    for (int i = 0; i < s_pin_len; i++) {
        text[2 * i] = '*';
        text[2 * i + 1] = ' ';
    }
    text[s_pin_len > 0 ? 2 * s_pin_len - 1 : 0] = '\0';
    text_center(LIST_Y + 24, s_pin_len > 0 ? text : "_", &font_12x24, C_ACCENT);
    for (int i = 0; i < PIN_ITEMS; i++) {
        const char *item = i == PIN_BACK ? "<" : i == PIN_OK ? "OK" : "0123456789" + i;
        int w = (i == PIN_OK ? 2 : 1) * font_12x24.w;
        bool cur = i == s_pin_cur;

        if (cur) {
            display_fill(x - PIN_GAP / 2, y - 2, w + PIN_GAP, font_12x24.h + 4, C_ACCENT);
        }
        if (i == PIN_OK) {
            display_text(x, y, item, &font_12x24, cur ? C_BG : C_TEXT, cur ? C_ACCENT : C_BG);
        } else {
            snprintf(text, sizeof(text), "%c", *item);
            display_text(x, y, text, &font_12x24, cur ? C_BG : C_TEXT, cur ? C_ACCENT : C_BG);
        }
        x += w + PIN_GAP;
    }
    display_text(4, FOOTER_Y, "Side: move  Front: pick", &font_8x16, C_DIM, C_BG);
    draw_clock();
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
        sound_play(SOUND_CLICK);
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
        show_error("No USB host", "Connect to a PC");
        return;
    }
    /* At once: the melody of the label plays while the device types. The
     * user hears that it types, and which entry. */
    sound_melody_start(label);
    show_message(enter ? "Typing + Enter" : "Typing", label, C_ACCENT);
    err = vault_secret(i, secret, sizeof(secret));
    if (err == ESP_OK) {
        err = usbdev_type(secret, enter);
    }
    vault_zero(secret, sizeof(secret));
    sound_melody_stop();
    if (err == ESP_OK) {
        show_message("Done", label, C_OK);
        vTaskDelay(pdMS_TO_TICKS(DONE_MS));
    } else if (err == ESP_ERR_INVALID_RESPONSE) {
        show_error("Caps Lock error", "Turn it off");
    } else {
        show_error("Typing failed", label);
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
    sound_play(SOUND_ERROR);
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
        show_error("No entries", NULL);
        return;
    }
    keypad_key_name(key, name);
    remove = vault_key(s_sel) == key;
    err = vault_set_key(s_sel, remove ? 0 : key);
    if (err != ESP_OK) {
        show_error("Write error", esp_err_to_name(err));
        return;
    }
    snprintf(title, sizeof(title), remove ? "Key %s removed" : "Key %s types", name);
    show_message(title, label, C_OK);
    sound_play(remove ? SOUND_OFF : SOUND_OK);
    vTaskDelay(pdMS_TO_TICKS(INFO_MS));
}

/* A tap of * turns the mouse mover on or off. It has no entry. The
 * detail shows the next change by the schedule: "Off at 21:00". */
static void toggle_mouse(void)
{
    char next[MOVER_NEXT_TEXT];
    bool on = !mover_on();

    mover_set(on);
    mover_next_text(next, sizeof(next));
    next[0] = toupper((unsigned char)next[0]);
    show_message(on ? "Mouse mover on" : "Mouse mover off",
                 next[0] ? next : on ? "PC stays awake" : "PC can sleep", C_OK);
    sound_play(on ? SOUND_ON : SOUND_OFF);
    vTaskDelay(pdMS_TO_TICKS(INFO_MS));
}

/* ---- PIN ---- */

static void pin_reset(void)
{
    vault_zero(s_pin, sizeof(s_pin));
    s_pin_len = 0;
    s_pin_cur = 0;
}

/* Check the digits so far. On success, the device is unlocked. */
static void pin_submit(void)
{
    char text[24];
    esp_err_t err;
    int wait;

    if (s_pin_len < VAULT_PIN_MIN) {
        snprintf(text, sizeof(text), "%d to %d digits", VAULT_PIN_MIN, VAULT_PIN_MAX);
        show_error("PIN too short", text);
        return;
    }
    wait = vault_wait_s();
    if (wait > 0) {
        snprintf(text, sizeof(text), "Wait %d s", wait);
        show_error("Too many tries", text);
        return;
    }
    show_message("Checking", NULL, C_ACCENT);
    err = vault_unlock(s_pin);
    pin_reset();
    if (err == ESP_OK) {
        show_message("Unlocked", NULL, C_OK);
        sound_play(SOUND_OK);
        vTaskDelay(pdMS_TO_TICKS(DONE_MS));
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(text, sizeof(text), "%d in a row", vault_fails());
        show_error("Wrong PIN", text);
    } else {
        show_error("Unlock error", esp_err_to_name(err));
    }
}

/* A digit, Backspace ('\b'), or Enter ('\n') for the PIN. */
static void pin_char(char c)
{
    if (c == '\n') {
        pin_submit();
    } else if (c == '\b') {
        if (s_pin_len > 0) {
            s_pin[--s_pin_len] = '\0';
        }
        sound_play(SOUND_CLICK);
    } else if (s_pin_len < VAULT_PIN_MAX) {
        s_pin[s_pin_len++] = c;
        sound_play(SOUND_CLICK);
    } else {
        sound_play(SOUND_ERROR);
    }
}

/* The front button picks the item under the cursor. */
static void pin_pick(void)
{
    if (s_pin_cur < 10) {
        pin_char('0' + s_pin_cur);
    } else {
        pin_char(s_pin_cur == PIN_BACK ? '\b' : '\n');
    }
}

/* Digit of a keypad key, or 0. */
static char keypad_digit(uint8_t key)
{
    if (key >= KEY_PAD_1 && key <= KEY_PAD_9) {
        return '1' + key - KEY_PAD_1;
    }
    if (key >= KEY_1 && key <= KEY_9) {
        return '1' + key - KEY_1;
    }
    return key == KEY_PAD_0 || key == KEY_0 ? '0' : 0;
}

/* The keypad while locked: digits, Enter, and Backspace go to the PIN.
 * Nothing goes to the USB host. */
static bool poll_pin_keypad(void)
{
    keypad_event_t ev;
    bool dirty = false;

    while (keypad_event(&ev)) {
        char c = keypad_digit(ev.key);

        dirty = true;
        if (ev.key == 0 || !ev.down) {
            continue;
        }
        if (c != 0) {
            pin_char(c);
        } else if (ev.key == KEY_BACKSPACE) {
            pin_char('\b');
        } else if (ev.key == KEY_ENTER || ev.key == KEY_PAD_ENTER) {
            pin_char('\n');
            break;  /* After an unlock, the next events are for the list. */
        }
    }
    return dirty;
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
            if (ev.down && !usbdev_ready()) {
                display_backlight(BRIGHTNESS);
                show_error("No USB host", "Connect to a PC");
                break;
            }
            /* A click as the key goes down: the key went to the computer. */
            if (usbdev_key(ev.key, ev.down) != ESP_OK) {
                sound_play(SOUND_ERROR);
            } else if (ev.down) {
                sound_play(SOUND_CLICK);
            }
        } else if (ev.key == KEY_PAD_MINUS || ev.key == KEY_PAD_PLUS) {
            if (ev.down) {
                select_step(ev.key == KEY_PAD_PLUS ? 1 : -1);
            }
        } else if (ev.down && s_kp_key == 0) {
            s_kp_key = ev.key;
            s_kp_ms = ev.ms;
            sound_prepare();    /* A tap types at the release. */
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
        if (held >= BIND_MS) {
            sound_play(SOUND_STEP);     /* Release now: the key gets the entry. */
        }
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
        show_error("Bluetooth error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err));
        return;
    }
    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();

        armed |= both == 0;
        keypad_pair_status(&pair);
        if (poll_flip() || first || pair.step != shown.step || pair.passkey != shown.passkey ||
            strcmp(pair.name, shown.name) != 0) {
            if (pair.step == KEYPAD_PAIR_PASSKEY && shown.step != pair.step) {
                sound_play(SOUND_STEP);     /* Type the passkey now. */
            }
            draw_pair(&pair);
            shown = pair;
            first = false;
        }
        if (pair.step == KEYPAD_PAIR_DONE || pair.step == KEYPAD_PAIR_FAILED) {
            sound_play(pair.step == KEYPAD_PAIR_DONE ? SOUND_OK : SOUND_ERROR);
            vTaskDelay(pdMS_TO_TICKS(pair.step == KEYPAD_PAIR_DONE ? INFO_MS : ERROR_MS));
            break;
        }
        if (side != PRESS_NONE || front != PRESS_NONE || (armed && both > 0) || s_dfu_request) {
            sound_play(SOUND_OFF);
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
    ntp_hold(true);     /* A time sync that runs stops: the mode needs the radio. */
    err = web_start(&s_web, buttons);
    if (err != ESP_OK) {
        ntp_hold(false);
        vault_zero(&s_web, sizeof(s_web));
        show_error("Wi-Fi error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err));
        return;
    }
    draw_web();
    sound_play(SOUND_ON);
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
        if (poll_flip()) {
            dirty = true;
        }
        if (connected != web_connected()) {
            connected = !connected;
            if (connected) {
                sound_play(SOUND_OK);   /* A phone or a laptop is on the network. */
            }
            dirty = true;
        }
        if (left != web_left_s()) {
            left = web_left_s();
            dirty = true;
        }
        if (dirty) {
            draw_web();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
    s_web_stop = false;
    show_message("Wi-Fi setup", "Stopping", C_ACCENT);
    sound_play(SOUND_OFF);
    web_stop();
    ntp_hold(false);
    vault_zero(&s_web, sizeof(s_web));
    show_message("Wi-Fi off", NULL, C_OK);
    vTaskDelay(pdMS_TO_TICKS(DONE_MS));
}

void ui_run(void)
{
    uint32_t generation = vault_generation();
    bool usb = usbdev_ready();
    keypad_state_t kp = keypad_state();
    int battery = keypad_battery();
    int minute = clock_minute();
    bool mouse = mover_on();
    bool screen_on = true;
    bool both_used = false;     /* The current push of both buttons did its action. */
    bool locked = vault_locked();
    int wait_s = 0;             /* Wait time on the PIN screen */
    uint32_t idle_ms = 0;

    /* The QR code library logs the text: it contains the Wi-Fi password. */
    esp_log_level_set("QRCODE", ESP_LOG_WARN);
    buttons_init();
    s_flip = load_setting("flip") != 0;
    display_set_flip(s_flip);
    /* The device starts the way it is held. Flat: the last orientation. */
    s_up = s_up_new = imu_up();
    s_up_count = 1;
    if (s_up != IMU_FLAT) {
        set_flip(s_up == IMU_BOTTOM_UP);
    }
    sound_enable(load_setting("mute") == 0);
    if (locked) {
        draw_pin();
    } else {
        draw_list();
    }
    display_backlight(BRIGHTNESS);

    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();
        uint32_t hold = 0;
        bool web = false, pair = false;
        bool dirty = false;
        int n;

        /* The action of a push comes at its release: the speaker is on then. */
        if (buttons_pushed()) {
            sound_prepare();
        }
        if (s_dfu_request) {
            show_message("Download mode", "Flash with idf.py", C_ACCENT);
            display_backlight(BRIGHTNESS);
            usbdev_dfu();
        }
        if (poll_flip()) {
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
        if (locked) {
            /* No modes while locked: the page and a pairing need the entries. */
            web = pair = false;
            hold = 0;
            s_web_start = false;
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
            /* A beep at 2 s and at 5 s: the user can hold without a look
             * at the screen. */
            if ((s_hold_ms < BOTH_MS && hold >= BOTH_MS) || (s_hold_ms < PAIR_MS && hold >= PAIR_MS)) {
                sound_play(SOUND_STEP);
            }
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
            if (!usb) {
                vault_lock();   /* The connection stopped: the next host must give the PIN. */
            }
            dirty = true;
        }
        if (locked != vault_locked()) {
            locked = !locked;
            pin_reset();
            idle_ms = 0;
            dirty = true;
        }
        if (kp != keypad_state()) {
            kp = keypad_state();
            dirty = true;
        }
        if (battery != keypad_battery()) {
            battery = keypad_battery();
            dirty = true;
        }
        if (mouse != mover_on()) {
            mouse = !mouse;
            dirty = true;
        }
        if (minute != clock_minute()) {
            minute = clock_minute();
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
        if (locked) {
            if (side != PRESS_NONE) {
                s_pin_cur = (s_pin_cur + (side == PRESS_SHORT ? 1 : PIN_ITEMS - 1)) % PIN_ITEMS;
                sound_play(SOUND_CLICK);
                dirty = true;
            }
            if (front == PRESS_SHORT) {
                pin_pick();
                dirty = true;
            } else if (front == PRESS_LONG) {
                pin_char('\n');
                dirty = true;
            }
            if (poll_pin_keypad()) {
                idle_ms = 0;
                dirty = true;
            }
            if (wait_s != vault_wait_s()) {
                wait_s = vault_wait_s();
                dirty = true;
            }
        } else {
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
        }

        if (dirty) {
            if (vault_locked()) {
                draw_pin();
            } else {
                draw_list();
            }
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
