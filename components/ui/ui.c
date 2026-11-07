/* Buttons, sounds, and the view. The side button selects an entry. The
 * front button types the password of the entry. Both buttons together
 * start the Wi-Fi setup mode, the pairing of a keypad, or the pairing of
 * a computer over Bluetooth. A keypad key types the entry that has this
 * key. The keypad keys - and + select an entry, and * turns the mouse
 * mover on or off. A sound tells the result of each action: the user does
 * not have to look at the device. The keys go to the Bluetooth computer
 * while it is connected, else to the USB host (hid.c).
 *
 * The view (view.h) shows the state: the LCD of the StickS3, or the key
 * LEDs of the Chain Dual Key. This file has the logic that is the same
 * for all boards.
 *
 * With a PIN, the device starts locked and locks again when the USB
 * connection or the link to the Bluetooth computer stops. The keypad
 * digits, Enter, and Backspace enter the PIN. The buttons also enter it,
 * in a way that fits the board (see "PIN" below). Nothing else works
 * while locked. */
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
#include "battery.h"
#include "blehid.h"
#include "buttons.h"
#include "clock.h"
#include "hid.h"
#include "keypad.h"
#include "mover.h"
#include "ntp.h"
#include "sound.h"
#include "usbdev.h"
#include "vault.h"
#include "web.h"
#include "view.h"
#include "ui.h"

#define IDLE_OFF_MS     60000   /* View dark after this idle time, on USB power */
#define IDLE_OFF_BAT_MS 20000   /* On battery: the backlight is the largest user. */
#define DONE_MS         600
#define ERROR_MS        1500
#define INFO_MS         1500    /* Message after a pairing or a key change */

/* Keypad keys that go to the computer: HID usage IDs */
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

static const char *TAG = "ui";
static volatile bool s_dfu_request;
static volatile bool s_web_start;
static volatile bool s_web_stop;
static volatile bool s_pair_request;
static ui_view_t s_v;                   /* The state that the view shows */
static char s_pin[VAULT_PIN_MAX + 1];   /* The digits so far */

void ui_flip(void)
{
    view_flip();
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

void ui_pair(void)
{
    s_pair_request = true;
}

uint8_t ui_setting(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;

    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

void ui_set_setting(const char *key, uint8_t v)
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
    ui_set_setting("mute", !on);
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

static void show_message(const char *title, const char *detail, view_tone_t tone)
{
    view_message(&s_v, title, detail, tone);
}

static void show_error(const char *title, const char *detail)
{
    show_message(title, detail, VIEW_ERROR);
    sound_play(SOUND_ERROR);
    vTaskDelay(pdMS_TO_TICKS(ERROR_MS));
}

static void show_state(void)
{
    if (vault_locked()) {
        view_pin(&s_v);
    } else {
        view_list(&s_v);
    }
}

/* ---- Actions ---- */

/* Select the next (1) or the previous (-1) entry. After the last entry
 * comes the first. */
static void select_step(int step)
{
    int n = vault_count();

    if (n > 0) {
        s_v.sel = (s_v.sel + step + n) % n;
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
    if (!hid_ready()) {
        show_error("No computer", "USB or Bluetooth");
        return;
    }
    /* At once: the melody of the label plays while the device types. The
     * user hears that it types, and which entry. */
    sound_melody_start(label);
    show_message(enter ? "Typing + Enter" : "Typing", label, VIEW_INFO);
    err = vault_secret(i, secret, sizeof(secret));
    if (err == ESP_OK) {
        err = hid_type(secret, enter);
    }
    vault_zero(secret, sizeof(secret));
    sound_melody_stop();
    if (err == ESP_OK) {
        show_message("Done", label, VIEW_OK);
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
    show_message(title, "Hold 2 s: bind", VIEW_INFO);
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

    if (vault_label(s_v.sel, label, sizeof(label)) != ESP_OK) {
        show_error("No entries", NULL);
        return;
    }
    keypad_key_name(key, name);
    remove = vault_key(s_v.sel) == key;
    err = vault_set_key(s_v.sel, remove ? 0 : key);
    if (err != ESP_OK) {
        show_error("Write error", esp_err_to_name(err));
        return;
    }
    snprintf(title, sizeof(title), remove ? "Key %s removed" : "Key %s types", name);
    show_message(title, label, VIEW_OK);
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
                 next[0] ? next : on ? "PC stays awake" : "PC can sleep", VIEW_OK);
    sound_play(on ? SOUND_ON : SOUND_OFF);
    vTaskDelay(pdMS_TO_TICKS(INFO_MS));
}

/* ---- PIN ---- */

static void pin_reset(void)
{
    vault_zero(s_pin, sizeof(s_pin));
    s_v.pin_len = 0;
    s_v.pin_cur = 0;
}

/* Check the digits so far. On success, the device is unlocked. */
static void pin_submit(void)
{
    char text[24];
    esp_err_t err;
    int wait;

    if (s_v.pin_len < VAULT_PIN_MIN) {
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
    show_message("Checking", NULL, VIEW_INFO);
    err = vault_unlock(s_pin);
    pin_reset();
    if (err == ESP_OK) {
        show_message("Unlocked", NULL, VIEW_OK);
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
        if (s_v.pin_len > 0) {
            s_pin[--s_v.pin_len] = '\0';
        }
        sound_play(SOUND_CLICK);
    } else if (s_v.pin_len < VAULT_PIN_MAX) {
        s_pin[s_v.pin_len++] = c;
        sound_play(SOUND_CLICK);
    } else {
        sound_play(SOUND_ERROR);
    }
}

#if CONFIG_HWPASS_LCD
/* The PIN screen shows a row of digits, delete, and OK. The side button
 * moves the cursor: a short push to the right, a long push to the left.
 * The front button picks the item under the cursor; a long push is OK. */
static void pin_buttons(press_t side, press_t front)
{
    if (side != PRESS_NONE) {
        s_v.pin_cur = (s_v.pin_cur + (side == PRESS_SHORT ? 1 : UI_PIN_ITEMS - 1)) % UI_PIN_ITEMS;
        sound_play(SOUND_CLICK);
    }
    if (front == PRESS_SHORT) {
        if (s_v.pin_cur < 10) {
            pin_char('0' + s_v.pin_cur);
        } else {
            pin_char(s_v.pin_cur == UI_PIN_BACK ? '\b' : '\n');
        }
    } else if (front == PRESS_LONG) {
        pin_char('\n');
    }
}
#elif CONFIG_HWPASS_BUTTONS == 1
/* One button, no screen: the user counts. Tap the button as many times
 * as the digit (ten times for 0), then hold it 0.6 s and release: the
 * digit goes in. The same hold without a tap deletes the last digit.
 * Hold 2 s and release: the device checks the PIN. ui_run gives the
 * holds as pushes of the side button: short before UI_BOTH_MS, long
 * after it. pin_cur counts the taps, 1 to 10; the eleventh tap starts
 * again at 1. */
static void pin_buttons(press_t side, press_t front)
{
    if (front == PRESS_SHORT) {
        s_v.pin_cur = s_v.pin_cur % 10 + 1;
        sound_play(SOUND_CLICK);
    }
    if (side == PRESS_SHORT) {
        if (s_v.pin_cur > 0) {
            pin_char('0' + s_v.pin_cur % 10);
        } else {
            pin_char('\b');
        }
        s_v.pin_cur = 0;
    } else if (side == PRESS_LONG) {
        s_v.pin_cur = 0;
        pin_char('\n');
    }
}
#else
/* Without a screen, the user counts: push the side key as many times as
 * the digit (ten times for 0), then push the front key: the digit goes
 * in. A long push of the front key checks the PIN. A long push of the
 * side key deletes the last digit. pin_cur counts the pushes, 1 to 10;
 * the eleventh push starts again at 1. Without a push, the front key
 * does nothing: a push by accident gives no digit. */
static void pin_buttons(press_t side, press_t front)
{
    if (side == PRESS_SHORT) {
        s_v.pin_cur = s_v.pin_cur % 10 + 1;
        sound_play(SOUND_CLICK);
    } else if (side == PRESS_LONG) {
        s_v.pin_cur = 0;
        pin_char('\b');
    }
    if (front == PRESS_SHORT) {
        if (s_v.pin_cur > 0) {
            pin_char('0' + s_v.pin_cur % 10);
            s_v.pin_cur = 0;
        }
    } else if (front == PRESS_LONG) {
        s_v.pin_cur = 0;
        pin_char('\n');
    }
}
#endif

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
 * Nothing goes to the computer. */
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

/* Enter and Backspace go to the computer as they are: after a password,
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
    static uint32_t kp_ms;      /* Time of the push of s_v.kp_key */

    while (keypad_event(&ev)) {
        dirty = true;
        if (ev.key == 0) {
            hid_key(0, false);
            s_v.kp_key = 0;
            s_v.kp_held = 0;
        } else if (passthru(ev.key)) {
            if (ev.down && !hid_ready()) {
                view_wake();
                show_error("No computer", "USB or Bluetooth");
                break;
            }
            /* A click as the key goes down: the key went to the computer. */
            if (hid_key(ev.key, ev.down) != ESP_OK) {
                sound_play(SOUND_ERROR);
            } else if (ev.down) {
                sound_play(SOUND_CLICK);
            }
        } else if (ev.key == KEY_PAD_MINUS || ev.key == KEY_PAD_PLUS) {
            if (ev.down) {
                select_step(ev.key == KEY_PAD_PLUS ? 1 : -1);
            }
        } else if (ev.down && s_v.kp_key == 0) {
            s_v.kp_key = ev.key;
            kp_ms = ev.ms;
            sound_prepare();    /* A tap types at the release. */
        } else if (!ev.down && ev.key == s_v.kp_key) {
            s_v.kp_key = 0;
            s_v.kp_held = 0;
            hid_key(0, false);
            view_wake();
            if (ev.key == KEY_PAD_STAR) {
                if (ev.ms - kp_ms < UI_BIND_MS) {
                    toggle_mouse();
                }
            } else if (ev.ms - kp_ms >= UI_BIND_MS) {
                bind_key(ev.key);
            } else {
                type_key(ev.key);
            }
            break;  /* Read the next events in the next cycle. */
        }
    }
    /* * cannot bind: no progress bar. */
    held = s_v.kp_key != 0 && s_v.kp_key != KEY_PAD_STAR ? now_ms() - kp_ms : 0;
    if (held > UI_BIND_MS) {
        held = UI_BIND_MS;      /* The bar is full: no more redraws. */
    }
    if (held / UI_HOLD_STEP_MS != s_v.kp_held / UI_HOLD_STEP_MS) {
        if (held >= UI_BIND_MS) {
            sound_play(SOUND_STEP);     /* Release now: the key gets the entry. */
        }
        s_v.kp_held = held;
        dirty = true;
    }
    return dirty;
}

/* ---- Modes ---- */

/* Pairing of a keypad. Return when it is done, when it fails, or when a
 * button stops it. */
static void pair_mode(void)
{
    keypad_pair_t pair, shown = { 0 };
    bool armed = false;     /* Both buttons were up after the start. */
    bool first = true;
    esp_err_t err;

    view_wake();
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
        if (view_poll() || first || pair.step != shown.step || pair.passkey != shown.passkey ||
            strcmp(pair.name, shown.name) != 0) {
            if (pair.step == KEYPAD_PAIR_PASSKEY && shown.step != pair.step) {
                sound_play(SOUND_STEP);     /* Type the passkey now. */
            }
            view_pair(&pair);
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

/* Pairing of a computer over Bluetooth. Return when it is done, when it
 * fails, or when a button stops it. */
static void pair_pc_mode(void)
{
    blehid_pair_t pair, shown = { 0 };
    bool armed = false;     /* Both buttons were up after the start. */
    bool first = true;
    esp_err_t err;

    view_wake();
    show_message("Pair computer", "Starting", VIEW_INFO);
    err = blehid_pair_start();
    if (err != ESP_OK) {
        show_error("Bluetooth error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err));
        return;
    }
    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();

        armed |= both == 0;
        blehid_pair_status(&pair);
        if (view_poll() || first || pair.step != shown.step || pair.passkey != shown.passkey ||
            strcmp(pair.name, shown.name) != 0) {
            if (pair.step == BLEHID_PAIR_PASSKEY && shown.step != pair.step) {
                sound_play(SOUND_STEP);     /* Type the passkey now. */
            }
            view_pair_pc(&pair);
            shown = pair;
            first = false;
        }
        if (pair.step == BLEHID_PAIR_DONE || pair.step == BLEHID_PAIR_FAILED) {
            bool done = pair.step == BLEHID_PAIR_DONE;

            sound_play(done ? SOUND_OK : SOUND_ERROR);
            /* The name of the computer can come after the pairing. */
            for (int ms = 0; ms < (done ? INFO_MS : ERROR_MS); ms += UI_HOLD_STEP_MS) {
                vTaskDelay(pdMS_TO_TICKS(UI_HOLD_STEP_MS));
                blehid_pair_status(&pair);
                if (strcmp(pair.name, shown.name) != 0) {
                    view_pair_pc(&pair);
                    shown = pair;
                }
            }
            break;
        }
        if (side != PRESS_NONE || front != PRESS_NONE || (armed && both > 0) || s_dfu_request) {
            sound_play(SOUND_OFF);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
    blehid_pair_stop();
}

/* Wi-Fi setup mode. Return when a button push, the page, the command
 * line, or the timeout stops it. buttons: the buttons started it. */
static void web_mode(bool buttons)
{
    bool armed = false;     /* Both buttons were up after the start. */
    bool connected = false;
    uint32_t left = 0;
    esp_err_t err;

    view_wake();
    show_message("Wi-Fi setup", "Starting", VIEW_INFO);
    ntp_hold(true);     /* A time sync that runs stops: the mode needs the radio. */
    err = web_start(&s_v.web, buttons);
    if (err != ESP_OK) {
        ntp_hold(false);
        vault_zero(&s_v.web, sizeof(s_v.web));
        show_error("Wi-Fi error", err == ESP_ERR_NOT_SUPPORTED ? "Not in this build" : esp_err_to_name(err));
        return;
    }
    view_web(&s_v);
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
        if (view_poll()) {
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
            view_web(&s_v);
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
    s_web_stop = false;
    show_message("Wi-Fi setup", "Stopping", VIEW_INFO);
    sound_play(SOUND_OFF);
    web_stop();
    ntp_hold(false);
    vault_zero(&s_v.web, sizeof(s_v.web));
    show_message("Wi-Fi off", NULL, VIEW_OK);
    vTaskDelay(pdMS_TO_TICKS(DONE_MS));
}

void ui_run(void)
{
    uint32_t generation = vault_generation();
    bool usb = usbdev_ready();
    bool ble = blehid_ready();
    blehid_state_t pc_state = blehid_state();
    keypad_state_t kp = keypad_state();
    int battery = keypad_battery();
    int minute = clock_minute();
    bool mouse = mover_on();
    bool awake = true;          /* The view is at full light. */
    bool both_used = false;     /* The current push of both buttons did its action. */
    bool locked = vault_locked();
    int wait_s = 0;             /* Wait time on the PIN screen */
    uint32_t idle_ms = 0;

    buttons_init();
    view_init();
    sound_enable(ui_setting("mute") == 0);
    show_state();
    view_wake();

    for (;;) {
        press_t side = buttons_poll(BUTTON_SIDE);
        press_t front = buttons_poll(BUTTON_FRONT);
        uint32_t both = buttons_both_ms();
        uint32_t hold = 0;
        bool web = false, pair = false, pair_pc = false;
        bool dirty = false;
        int n;

        /* The action of a push comes at its release: the speaker is on then. */
        if (buttons_pushed()) {
            sound_prepare();
        }
        if (s_dfu_request) {
            show_message("Download mode", "Flash with idf.py", VIEW_INFO);
            view_wake();
            usbdev_dfu();
        }
        if (view_poll()) {
            dirty = true;
        }
        /* Hold both buttons, then release them. The last hold time on the
         * view selects the mode. */
        if (both == 0) {
            web = !both_used && s_v.hold_ms >= UI_BOTH_MS && s_v.hold_ms < UI_PAIR_MS;
            pair = !both_used && s_v.hold_ms >= UI_PAIR_MS && s_v.hold_ms < UI_HOST_MS;
            pair_pc = !both_used && s_v.hold_ms >= UI_HOST_MS;
#if CONFIG_HWPASS_BUTTONS == 1
            /* One button: a hold is a push of the side button. Released
             * before 2 s, a short push: the next entry, or a PIN digit.
             * Locked and released after 2 s, a long push: the PIN check.
             * Unlocked, the release after 2 s starts a mode. */
            if (!both_used && s_v.hold_ms > 0 && s_v.hold_ms < UI_BOTH_MS) {
                side = PRESS_SHORT;
            } else if (locked && !both_used && s_v.hold_ms >= UI_BOTH_MS) {
                side = PRESS_LONG;
            }
#endif
            both_used = false;
        } else if (!both_used) {
            hold = both;
        }
        if (locked) {
            /* No modes while locked: the page and a pairing need the entries. */
            web = pair = pair_pc = false;
#if CONFIG_HWPASS_BUTTONS != 1
            hold = 0;   /* One button: the PIN entry has holds, and the view shows them. */
#endif
            s_web_start = s_pair_request = false;
        }
        s_web_stop = false;     /* The mode is off. */
        if (s_web_start || s_pair_request || web || pair || pair_pc) {
            /* A request from the command line counts first: the USB host
             * gets the Wi-Fi password. */
            bool cli = s_web_start;

            s_web_start = false;
            s_v.hold_ms = hold = 0;
            hid_key(0, false);
            if (cli || web) {
                web_mode(!cli);
            } else if (pair) {
                pair_mode();
            } else {
                s_pair_request = false;
                pair_pc_mode();
            }
            /* The buttons and the keypad were not read during the mode and
             * the messages. A push in this time must not type. Both
             * buttons can still be down: this push is used. */
            buttons_consume();
            keypad_flush();
            s_v.kp_key = 0;
            s_v.kp_held = 0;
            both_used = true;
            side = front = PRESS_NONE;
            awake = true;
            idle_ms = 0;
            dirty = true;
        }
        if (hold / UI_HOLD_STEP_MS != s_v.hold_ms / UI_HOLD_STEP_MS) {
            /* A beep at 2 s, 5 s, and 8 s: the user can hold without a
             * look at the device. */
            if ((s_v.hold_ms < UI_BOTH_MS && hold >= UI_BOTH_MS) ||
                (s_v.hold_ms < UI_PAIR_MS && hold >= UI_PAIR_MS) ||
                (s_v.hold_ms < UI_HOST_MS && hold >= UI_HOST_MS)) {
                sound_play(SOUND_STEP);
            }
            s_v.hold_ms = hold;
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
        if (s_v.sel >= n) {
            s_v.sel = n > 0 ? n - 1 : 0;
        }
        if (usb != usbdev_ready() || ble != blehid_ready()) {
            /* A link stopped: the next computer must give the PIN. */
            if ((usb && !usbdev_ready()) || (ble && !blehid_ready())) {
                vault_lock();
            }
            usb = usbdev_ready();
            ble = blehid_ready();
            dirty = true;
        }
        if (pc_state != blehid_state()) {
            pc_state = blehid_state();
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
            /* When the view is dark, a push only wakes it. */
            if (!awake) {
                side = front = PRESS_NONE;
                dirty = true;
            }
        }
        if (locked) {
            if (side != PRESS_NONE || front != PRESS_NONE) {
                pin_buttons(side, front);
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
                type_entry(s_v.sel, front == PRESS_LONG);
                dirty = true;
            }
            /* A keypad key works also when the view is dark. */
            if (poll_keypad()) {
                idle_ms = 0;
                dirty = true;
            }
        }

        if (dirty) {
            show_state();
        }
        if (idle_ms == 0 && !awake) {
            awake = true;
            view_wake();
        }
        idle_ms += BUTTONS_POLL_MS;
        if (awake && idle_ms >= (battery_usb() ? IDLE_OFF_MS : IDLE_OFF_BAT_MS)) {
            awake = false;
            view_sleep();
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTONS_POLL_MS));
    }
}
