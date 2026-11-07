/* The view: the part of the UI that shows the state to the user. One
 * file implements it for each kind of board (CMakeLists.txt picks it):
 *
 *   view_lcd.c   The LCD of the StickS3, with the tilt sensor.
 *   view_led.c   The two RGB key LEDs of the Chain Dual Key. Texts that
 *                the user must read (a passkey, the Wi-Fi password) go
 *                to the serial port.
 *
 * ui.c calls the view from the UI task only. The view reads the modules
 * (vault, keypad, hid, ...) itself; ui.c gives it the state that only
 * ui.c has, in ui_view_t. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "blehid.h"
#include "keypad.h"
#include "web.h"

/* Hold times of both buttons: the view shows the step. */
#define UI_BOTH_MS      2000    /* Hold both buttons, then release: Wi-Fi setup mode. */
#define UI_PAIR_MS      5000    /* Hold both buttons longer: pair a keypad. */
#define UI_HOST_MS      8000    /* Hold both buttons still longer: pair a computer. */
#define UI_BIND_MS      2000    /* Hold a keypad key: it gets the selected entry. */
#define UI_HINT_MS      300     /* Keypad hold time before the view shows the hold */
#define UI_HOLD_STEP_MS 100     /* Update interval of a hold */

/* The PIN screen of the LCD: the items of the row. 0 to 9, then delete and OK. */
#define UI_PIN_ITEMS    12
#define UI_PIN_BACK     10
#define UI_PIN_OK       11

typedef struct {
    int sel;                /* The selected entry */
    uint32_t hold_ms;       /* Both buttons are down for this time, else 0 */
    uint8_t kp_key;         /* The keypad key that is down, or 0 */
    uint32_t kp_held;       /* Its hold time, in steps of UI_HOLD_STEP_MS */
    int pin_len;            /* PIN digits so far */
    int pin_cur;            /* LCD: the item under the cursor. LEDs: pushes for the next digit. */
    web_info_t web;         /* Wi-Fi setup mode: the network */
} ui_view_t;

/* The kind of a message: the color. */
typedef enum {
    VIEW_INFO,      /* An action runs: "Typing", "Checking". */
    VIEW_OK,        /* An action is complete. */
    VIEW_ERROR,
} view_tone_t;

/* Start the view. Call after the modules started, before the first show. */
esp_err_t view_init(void);

/* Call each UI cycle (BUTTONS_POLL_MS). Return true when the view must
 * show its state again (the screen turned). */
bool view_poll(void);

/* Turn the screen 180 degrees ("rotate"). A board without a screen does nothing. */
void view_flip(void);

/* The user acts: the screen or the LEDs go to full light. */
void view_wake(void);

/* No activity for a long time: the screen goes dark, the LEDs dim. */
void view_sleep(void);

/* The entry list with the state of the device, or the PIN screen. */
void view_list(const ui_view_t *v);
void view_pin(const ui_view_t *v);

/* A message, until the next show. detail can be NULL. */
void view_message(const ui_view_t *v, const char *title, const char *detail, view_tone_t tone);

/* The modes. ui.c calls them when the state changes. */
void view_web(const ui_view_t *v);
void view_pair(const keypad_pair_t *p);
void view_pair_pc(const blehid_pair_t *p);

/* Settings of the UI in NVS, namespace "ui": "flip", "mute". Default 0.
 * In ui.c, for the views. */
uint8_t ui_setting(const char *key);
void ui_set_setting(const char *key, uint8_t v);
