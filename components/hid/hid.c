/* Keyboard and mouse output on the USB link (usbdev) or the Bluetooth LE
 * link (blehid). The Bluetooth computer gets the keys while it is
 * connected: the user turned this link on with a pairing. Else the USB
 * host gets them. Each report goes to the link with a delay after it. The
 * key codes come from the TinyUSB HID header: they are the same on both
 * links. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "class/hid/hid.h"
#include "blehid.h"
#include "usbdev.h"
#include "hid.h"

#define KEY_DELAY_MS    10      /* After each report. RDP and slow hosts lose fast keys. */
#define CAPS_HOLD_MS    200     /* macOS ignores short Caps Lock pushes. */
#define CAPS_WAIT_MS    500     /* Time for the computer to send the new LED state. */
#define LAYOUT_WAIT_MS  150     /* Time for the computer to change the key layout */
#define NVS_NAMESPACE   "usb"   /* The layout key was a setting of usbdev: keep its place. */
#define NVS_LAYOUT      "layout"    /* u16: modifiers in the high byte, key in the low byte */
#define MOUSE_PERIOD_US (HID_MOUSE_PERIOD_S * 1000000LL)

/* HID usage IDs of the layout key names */
#define KEY_A           0x04
#define KEY_1           0x1E
#define KEY_0           0x27
#define KEY_SPACE       0x2C
#define KEY_F1          0x3A
#define KEY_F13         0x68

static const char *TAG = "hid";
static SemaphoreHandle_t s_lock;        /* Typing, s_held, and mouse moves */
static uint8_t s_held[6];               /* Keys that hid_key holds down */
static bool s_held_lost;                /* The computer did not get s_held. */
static volatile bool s_mouse_on;
static int8_t s_mouse_dx = 1;           /* Next move: right, then back to the left */
static uint8_t s_layout_mod;            /* Layout key: modifiers and key, 0: none */
static uint8_t s_layout_key;

static const struct {
    const char *name;
    uint8_t mod;
} s_mods[] = {
    { "Ctrl", KEYBOARD_MODIFIER_LEFTCTRL },
    { "Shift", KEYBOARD_MODIFIER_LEFTSHIFT },
    { "Alt", KEYBOARD_MODIFIER_LEFTALT },
    { "Win", KEYBOARD_MODIFIER_LEFTGUI },
};

/* ---- Links ---- */

hid_link_t hid_link(void)
{
    if (blehid_ready()) {
        return HID_LINK_BLE;
    }
    return usbdev_ready() ? HID_LINK_USB : HID_LINK_NONE;
}

bool hid_ready(void)
{
    return hid_link() != HID_LINK_NONE;
}

static bool caps_lock(hid_link_t link)
{
    return link == HID_LINK_BLE ? blehid_caps_lock() : usbdev_caps_lock();
}

bool hid_caps_lock(void)
{
    return caps_lock(hid_link());
}

static bool send_keys(hid_link_t link, uint8_t mod, const uint8_t keys[6])
{
    bool ok;

    switch (link) {
    case HID_LINK_BLE:
        ok = blehid_send_keys(mod, keys) == ESP_OK;
        break;
    case HID_LINK_USB:
        ok = usbdev_send_keys(mod, keys);
        break;
    default:
        ok = false;
        break;
    }
    if (!ok) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_DELAY_MS));
    return true;
}

/* ---- Keyboard ---- */

static bool send_report(hid_link_t link, uint8_t mod, uint8_t key)
{
    uint8_t keys[6] = { key };

    return send_keys(link, mod, keys);
}

/* Some computers need the modifier in an earlier report than the key. */
static bool push_key(hid_link_t link, uint8_t mod, uint8_t key, int hold_ms)
{
    if (mod && !send_report(link, mod, 0)) {
        return false;
    }
    if (!send_report(link, mod, key)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return send_report(link, 0, 0);
}

/* Push Caps Lock, then wait until the computer reports the new state.
 * Returns false if the LED state does not change. */
static bool set_caps_lock(hid_link_t link, bool on)
{
    if (!push_key(link, 0, HID_KEY_CAPS_LOCK, CAPS_HOLD_MS)) {
        return false;
    }
    for (int ms = 0; caps_lock(link) != on; ms += 10) {
        if (ms >= CAPS_WAIT_MS) {
            ESP_LOGW(TAG, "Caps Lock LED did not change");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

esp_err_t hid_type(const char *s, bool enter)
{
    static const uint8_t ascii[128][2] = { HID_ASCII_TO_KEYCODE };
    hid_link_t link;
    bool caps, ok = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    link = hid_link();
    if (link == HID_LINK_NONE) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_held, 0, sizeof(s_held));  /* The next report releases them. */
    s_held_lost = false;
    /* If Caps Lock does not turn off, the state of the computer is
     * unknown. Do not type: the text can have the wrong case. The second
     * push below restores the state. */
    caps = caps_lock(link);
    if (caps && !set_caps_lock(link, false)) {
        set_caps_lock(link, true);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s_layout_mod || s_layout_key) {
        ok = push_key(link, s_layout_mod, s_layout_key, 0);
        vTaskDelay(pdMS_TO_TICKS(LAYOUT_WAIT_MS));
    }
    for (; ok && *s; s++) {
        unsigned char c = *s;
        if (c >= 0x20 && c <= 0x7E) {
            ok = push_key(link, ascii[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, ascii[c][1], 0);
        }
    }
    if (ok && enter) {
        ok = push_key(link, 0, HID_KEY_ENTER, 0);
    }
    if (!ok) {
        /* A failed push can leave a key down. The computer then repeats it. */
        s_held_lost = !send_report(link, 0, 0);
    }
    if (caps) {
        set_caps_lock(link, true);
    }
    xSemaphoreGive(s_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t hid_key(uint8_t key, bool down)
{
    uint8_t keys[6] = { 0 };
    bool ok = true;
    int n = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < 6; i++) {
        if (key != 0 && s_held[i] != 0 && s_held[i] != key) {
            keys[n++] = s_held[i];
        }
    }
    if (key != 0 && down && n < 6) {
        keys[n] = key;
    }
    /* Send only a change: the UI releases all keys before each action.
     * After an error, send again: a lost release makes the computer repeat. */
    if (memcmp(keys, s_held, sizeof(keys)) != 0 || s_held_lost) {
        memcpy(s_held, keys, sizeof(keys));
        ok = send_keys(hid_link(), 0, keys);
        s_held_lost = !ok;
    }
    xSemaphoreGive(s_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---- Layout key ---- */

/* Key code of a key name, or 0. */
static uint8_t key_code(const char *name)
{
    size_t n = strlen(name);
    char *end;
    long f;

    if (n == 1 && name[0] >= '1' && name[0] <= '9') {
        return KEY_1 + name[0] - '1';
    }
    if (n == 1 && name[0] == '0') {
        return KEY_0;
    }
    if (n == 1 && isalpha((unsigned char)name[0])) {
        return KEY_A + tolower((unsigned char)name[0]) - 'a';
    }
    if (strcasecmp(name, "space") == 0) {
        return KEY_SPACE;
    }
    if (tolower((unsigned char)name[0]) == 'f' && isdigit((unsigned char)name[1])) {
        f = strtol(name + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) {
            return KEY_F1 + f - 1;
        }
        if (*end == '\0' && f >= 13 && f <= 24) {
            return KEY_F13 + f - 13;
        }
    }
    return 0;
}

/* Name of a key code of key_code. */
static void key_name(uint8_t key, char *buf, size_t size)
{
    if (key >= KEY_1 && key < KEY_0) {
        snprintf(buf, size, "%c", '1' + key - KEY_1);
    } else if (key == KEY_0) {
        snprintf(buf, size, "0");
    } else if (key >= KEY_A && key < KEY_A + 26) {
        snprintf(buf, size, "%c", 'A' + key - KEY_A);
    } else if (key == KEY_SPACE) {
        snprintf(buf, size, "Space");
    } else if (key >= KEY_F1 && key < KEY_F1 + 12) {
        snprintf(buf, size, "F%d", key - KEY_F1 + 1);
    } else if (key >= KEY_F13 && key < KEY_F13 + 12) {
        snprintf(buf, size, "F%d", key - KEY_F13 + 13);
    } else {
        snprintf(buf, size, "?");
    }
}

/* "off", "F16", or "ctrl+shift+1": modifiers and at most one key. */
static bool parse_layout(const char *text, uint8_t *mod, uint8_t *key)
{
    char buf[HID_LAYOUT_TEXT * 2], *tok, *save;

    *mod = *key = 0;
    if (strcasecmp(text, "off") == 0) {
        return true;
    }
    if (strlcpy(buf, text, sizeof(buf)) >= sizeof(buf)) {
        return false;
    }
    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        size_t i;

        for (i = 0; i < sizeof(s_mods) / sizeof(s_mods[0]); i++) {
            if (strcasecmp(tok, s_mods[i].name) == 0) {
                *mod |= s_mods[i].mod;
                break;
            }
        }
        if (i < sizeof(s_mods) / sizeof(s_mods[0])) {
            continue;
        }
        if (*key != 0 || (*key = key_code(tok)) == 0) {
            return false;
        }
    }
    return *mod != 0 || *key != 0;
}

esp_err_t hid_set_layout(const char *text)
{
    nvs_handle_t h;
    uint8_t mod, key;
    esp_err_t err;

    if (!parse_layout(text, &mod, &key)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, NVS_LAYOUT, (uint16_t)mod << 8 | key);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the layout key: %s", esp_err_to_name(err));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_layout_mod = mod;
    s_layout_key = key;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void hid_layout_text(char *buf, size_t size)
{
    uint8_t mod = s_layout_mod, key = s_layout_key;
    size_t n = 0;

    if (mod == 0 && key == 0) {
        strlcpy(buf, "off", size);
        return;
    }
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(s_mods) / sizeof(s_mods[0]); i++) {
        if (mod & s_mods[i].mod) {
            n += snprintf(buf + n, size - n, "%s+", s_mods[i].name);
        }
    }
    if (key != 0) {
        key_name(key, buf + n, size - n);
    } else if (n > 0) {
        buf[n - 1] = '\0';  /* Only modifiers: no "+" at the end. */
    }
}

static void load_layout(void)
{
    nvs_handle_t h;
    uint16_t v = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NVS_LAYOUT, &v);
        nvs_close(h);
    }
    s_layout_mod = v >> 8;
    s_layout_key = v & 0xFF;
}

/* ---- Mouse mover ---- */

/* esp_timer task: do not wait. If a move cannot go now, it goes in the
 * next period. No move while the device types: with "focus follows
 * mouse", a move can change the window that gets the keys. */
static void mouse_tick(void *arg)
{
    hid_link_t link = hid_link();
    bool ok;

    if (!s_mouse_on || link == HID_LINK_NONE || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (link == HID_LINK_BLE) {
        ok = blehid_send_mouse(s_mouse_dx, 0) == ESP_OK;
    } else {
        ok = usbdev_send_mouse(s_mouse_dx, 0);
    }
    if (ok) {
        s_mouse_dx = -s_mouse_dx;
    }
    xSemaphoreGive(s_lock);
}

void hid_mouse_mover(bool on)
{
    s_mouse_on = on;
}

bool hid_mouse_mover_on(void)
{
    return s_mouse_on;
}

/* ---- Start ---- */

esp_err_t hid_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = mouse_tick,
        .name = "mouse",
    };
    esp_timer_handle_t timer;

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    load_layout();
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, MOUSE_PERIOD_US), TAG, "timer start");
    return ESP_OK;
}
