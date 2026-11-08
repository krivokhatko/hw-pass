/* The view on the LCD of the StickS3 and the StickC Plus. The screen turns
 * with the device: the tilt sensor tells which edge is up. When the device
 * lies flat, the screen keeps its orientation, and the "rotate" command
 * turns it. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "qrcode.h"
#include "clock.h"
#include "display.h"
#include "hid.h"
#include "imu.h"
#include "mover.h"
#include "vault.h"
#include "view.h"

#define BRIGHTNESS      160
#define BATTERY_LOW     20      /* Keypad battery level in %: red at this level and below */
#define TILT_MS         100     /* Read interval of the tilt sensor */
#define TILT_STABLE     5       /* Reads in a row with the same result before the screen turns: 0.5 s */
#define PIN_GAP         6       /* Pixels between the items of the PIN row */

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

static volatile bool s_flip_request;
static bool s_flip;
static imu_up_t s_up;           /* The stable result of the tilt sensor */
static imu_up_t s_up_new;       /* The last result, and how many reads in a row gave it */
static int s_up_count;
static uint32_t s_up_ms;        /* Time of the last read */

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

static void set_flip(bool flip)
{
    if (flip != s_flip) {
        s_flip = flip;
        ui_set_setting("flip", s_flip);
    }
    display_set_flip(s_flip);
}

esp_err_t view_init(void)
{
    /* The QR code library logs the text: it contains the Wi-Fi password. */
    esp_log_level_set("QRCODE", ESP_LOG_WARN);
    s_flip = ui_setting("flip") != 0;
    display_set_flip(s_flip);
    /* The device starts the way it is held. Flat: the last orientation. */
    s_up = s_up_new = imu_up();
    s_up_count = 1;
    if (s_up != IMU_FLAT) {
        set_flip(s_up == IMU_BOTTOM_UP);
    }
    return ESP_OK;
}

void view_flip(void)
{
    s_flip_request = true;
}

/* The command line asks for a turn, or the device was turned. The tilt
 * sensor reads each TILT_MS, and a new result must hold for TILT_STABLE
 * reads. Only a change of the result turns the screen: a turn by the
 * command line holds until the device is turned. When the device lies
 * flat, the sensor gives no edge. Return true when the screen turned. */
bool view_poll(void)
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

void view_wake(void)
{
    display_backlight(BRIGHTNESS);
}

void view_sleep(void)
{
    display_backlight(0);
}

/* ---- Drawing ---- */

static uint16_t tone_color(view_tone_t tone)
{
    return tone == VIEW_OK ? C_OK : tone == VIEW_ERROR ? C_ERROR : C_ACCENT;
}

static void text_center(int y, const char *s, const font_t *f, uint16_t fg)
{
    display_text((DISPLAY_W - (int)strlen(s) * f->w) / 2, y, s, f, fg, C_BG);
}

/* The header items from the right: the link that gets the keys (USB or
 * BLE), keypad and its battery level, mouse mover, position. With all
 * items, the title just fits: "hw-pass 32/32  MM BT 100% USB". */
static void draw_frame(int sel)
{
    char text[24];
    int n = vault_count();
    int x = DISPLAY_W - 4 - 3 * font_8x16.w;
    keypad_state_t kp = keypad_state();
    int battery = keypad_battery();
    hid_link_t link = hid_link();
    /* "BLE": the computer gets the keys, or no link is ready and a
     * computer is paired (gray). "USB": the host gets the keys, or no
     * link is ready and no computer is paired (red). A board without
     * USB: "BLE" always; red without a paired computer. */
#if CONFIG_HWPASS_USB
    bool ble = link == HID_LINK_BLE || (link == HID_LINK_NONE && blehid_state() != BLEHID_NONE);
#else
    bool ble = true;
#endif

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "hw-pass", &font_8x16, C_DIM, C_BG);
    display_text(x, HEADER_Y, ble ? "BLE" : "USB", &font_8x16,
                 link != HID_LINK_NONE ? C_OK : blehid_state() != BLEHID_NONE ? C_DIM : C_ERROR, C_BG);
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
        display_text(x, HEADER_Y, "MM", &font_8x16, hid_ready() ? C_OK : C_DIM, C_BG);
    }
    if (n > 0) {
        snprintf(text, sizeof(text), "%d/%d", sel + 1, n);
        display_text(x - (2 + (int)strlen(text)) * font_8x16.w, HEADER_Y, text, &font_8x16, C_TEXT, C_BG);
    }
    display_fill(0, LIST_Y - 3, DISPLAY_W, 1, C_LINE);
    display_fill(0, FOOTER_Y - 2, DISPLAY_W, 1, C_LINE);
}

/* The footer during a hold of both buttons: 2 s, then 3 s more, then 3 s
 * more. */
static void draw_both_hold(uint32_t hold_ms)
{
    if (hold_ms < UI_BOTH_MS) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * hold_ms / UI_BOTH_MS, 2, C_ACCENT);
        display_text(4, FOOTER_Y, "Hold for Wi-Fi setup", &font_8x16, C_ACCENT, C_BG);
    } else if (hold_ms < UI_PAIR_MS) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * (hold_ms - UI_BOTH_MS) / (UI_PAIR_MS - UI_BOTH_MS), 2, C_BT);
        display_text(4, FOOTER_Y, "Release: Wi-Fi. Hold: keypad", &font_8x16, C_ACCENT, C_BG);
    } else if (hold_ms < UI_HOST_MS) {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * (hold_ms - UI_PAIR_MS) / (UI_HOST_MS - UI_PAIR_MS), 2, C_BT);
        display_text(4, FOOTER_Y, "Release: keypad. Hold: PC", &font_8x16, C_BT, C_BG);
    } else {
        display_fill(0, FOOTER_Y - 3, DISPLAY_W, 2, C_BT);
        display_text(4, FOOTER_Y, "Release: pair computer", &font_8x16, C_BT, C_BG);
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

void view_list(const ui_view_t *v)
{
    char label[VAULT_LABEL_MAX + 1], name[KEYPAD_KEY_NAME], text[32];
    int n = vault_count();
    int first = v->sel - 1;  /* One entry of context above the selection. */

    draw_frame(v->sel);
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
        uint16_t fg = i == v->sel ? C_BG : C_TEXT, bg = i == v->sel ? C_ACCENT : C_BG;
        uint8_t key = vault_key(i);
        int right = DISPLAY_W;  /* The label ends before the key name. */

        display_fill(0, y, DISPLAY_W, ROW_H, bg);
        if (key != 0) {
            keypad_key_name(key, name);
            right = DISPLAY_W - 4 - (int)strlen(name) * font_8x16.w;
            display_text(right, y + 4, name, &font_8x16, i == v->sel ? C_BG : C_DIM, bg);
        }
        if (vault_label(i, label, sizeof(label)) == ESP_OK) {
            int max = (right - 6 - 4) / font_12x24.w;

            if ((int)strlen(label) > max) {
                label[max] = '\0';
            }
            display_text(6, y, label, &font_12x24, fg, bg);
        }
    }
    if (v->hold_ms > 0) {
        draw_both_hold(v->hold_ms);
    } else if (v->kp_held >= UI_HINT_MS) {
        keypad_key_name(v->kp_key, name);
        snprintf(text, sizeof(text), v->kp_held >= UI_BIND_MS ? "Release: bind key %s" : "Hold to bind key %s",
                 name);
        display_fill(0, FOOTER_Y - 3, DISPLAY_W * v->kp_held / UI_BIND_MS, 2, C_BT);
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
void view_web(const ui_view_t *v)
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
        snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", v->web.ssid, v->web.password);
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
        display_text(WEB_X, 22, v->web.ssid, &font_8x16, C_TEXT, C_BG);
        snprintf(text, sizeof(text), "%.8s", v->web.password);
        display_text(WEB_X, 44, text, &font_12x24, C_TEXT, C_BG);
        display_text(WEB_X, 68, v->web.password + 8, &font_12x24, C_TEXT, C_BG);
        display_text(WEB_X, 98, "Scan the QR", &font_8x16, C_DIM, C_BG);
    }
    display_text(WEB_X, 117, "Button: stop", &font_8x16, C_DIM, C_BG);
    display_flush();
    vault_zero(text, sizeof(text));
}

void view_message(const ui_view_t *v, const char *title, const char *detail, view_tone_t tone)
{
    draw_frame(v->sel);
    text_center(LIST_Y + 14, title, &font_12x24, tone_color(tone));
    if (detail) {
        text_center(LIST_Y + 50, detail, &font_12x24, C_TEXT);
    }
    display_flush();
}

/* The PIN screen: a line of state, the digits so far as stars, and the
 * row of items with the cursor. */
void view_pin(const ui_view_t *v)
{
    char text[32];      /* Also the stars: 2 * VAULT_PIN_MAX + 1 */
    int wait = vault_wait_s();
    int x = (DISPLAY_W - (10 * (font_12x24.w + PIN_GAP) + font_12x24.w + PIN_GAP + 2 * font_12x24.w)) / 2;
    int y = LIST_Y + 56;

    draw_frame(v->sel);
    if (wait > 0) {
        snprintf(text, sizeof(text), "Wrong PIN. Wait %d s", wait);
        text_center(LIST_Y + 2, text, &font_8x16, C_ERROR);
    } else if (vault_fails() > 0) {
        text_center(LIST_Y + 2, "Wrong PIN. Try again", &font_8x16, C_ERROR);
    } else {
        text_center(LIST_Y + 2, "Enter the PIN", &font_8x16, C_TEXT);
    }
    for (int i = 0; i < v->pin_len; i++) {
        text[2 * i] = '*';
        text[2 * i + 1] = ' ';
    }
    text[v->pin_len > 0 ? 2 * v->pin_len - 1 : 0] = '\0';
    text_center(LIST_Y + 24, v->pin_len > 0 ? text : "_", &font_12x24, C_ACCENT);
    for (int i = 0; i < UI_PIN_ITEMS; i++) {
        const char *item = i == UI_PIN_BACK ? "<" : i == UI_PIN_OK ? "OK" : "0123456789" + i;
        int w = (i == UI_PIN_OK ? 2 : 1) * font_12x24.w;
        bool cur = i == v->pin_cur;

        if (cur) {
            display_fill(x - PIN_GAP / 2, y - 2, w + PIN_GAP, font_12x24.h + 4, C_ACCENT);
        }
        if (i == UI_PIN_OK) {
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

void view_pair(const keypad_pair_t *p)
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

void view_pair_pc(const blehid_pair_t *p)
{
    char text[16];

    display_fill(0, 0, DISPLAY_W, DISPLAY_H, C_BG);
    display_text(4, HEADER_Y, "Pair computer", &font_8x16, C_BT, C_BG);
    switch (p->step) {
    case BLEHID_PAIR_WAITING:
        text_center(LIST_Y + 4, "Waiting", &font_12x24, C_ACCENT);
        text_center(LIST_Y + 40, "Add \"hw-pass\" in the", &font_8x16, C_TEXT);
        text_center(LIST_Y + 58, "Bluetooth settings", &font_8x16, C_TEXT);
        break;
    case BLEHID_PAIR_PASSKEY:
        snprintf(text, sizeof(text), "%06lu", (unsigned long)p->passkey);
        text_center(LIST_Y, "Type on the computer:", &font_8x16, C_TEXT);
        text_center(LIST_Y + 22, text, &font_12x24, C_ACCENT);
        text_center(LIST_Y + 54, "then Enter", &font_8x16, C_TEXT);
        break;
    case BLEHID_PAIR_DONE:
        text_center(LIST_Y + 4, "Computer paired", &font_12x24, C_OK);
        text_center(LIST_Y + 40, p->name, &font_8x16, C_TEXT);
        break;
    case BLEHID_PAIR_FAILED:
        text_center(LIST_Y + 4, "Pairing failed", &font_12x24, C_ERROR);
        text_center(LIST_Y + 40, p->error ? p->error : "", &font_8x16, C_TEXT);
        break;
    }
    if (p->step != BLEHID_PAIR_DONE && p->step != BLEHID_PAIR_FAILED) {
        display_text(4, FOOTER_Y, "Button: stop", &font_8x16, C_DIM, C_BG);
    }
    display_flush();
}
