/* The view on the RGB LEDs: the two key LEDs of the Chain Dual Key, or
 * the one LED of the StampS3A. The LED under the side key shows the link
 * to the computer; the LED under the front key shows the selected entry
 * by its color. Blinks and flashes show the modes, the holds, and the PIN
 * pushes. A text that the user must read (a passkey, the Wi-Fi network
 * and password, a result) goes to the serial port.
 *
 * One LED (LEDS_COUNT == 1): the LED is the side LED and the front LED
 * at the same time. It shows the entry color while a computer gets the
 * keys, with a blue tick every 2 s when this is the Bluetooth computer.
 * Without entries, or without a computer, it shows the link. A hold of
 * the button on the PIN screen: amber, release for the digit; cyan after
 * 2 s, release for the check.
 *
 * Side LED:  green, the USB host gets the keys. Blue, the Bluetooth
 *            computer gets the keys. Blue blink, a computer is paired
 *            but not connected. Red, no computer is ready.
 * Front LED: the color of the selected entry: eight colors in the order
 *            of the list, then the same colors again. Off, no entries.
 *            Amber blink, the device is locked; red blink, wait after
 *            wrong PINs.
 * Both:      amber that gets brighter, hold both keys; cyan, release for
 *            the Wi-Fi setup mode; blue, release to pair a keypad;
 *            purple, release to pair a computer. Green, an action is
 *            complete. Red, an error. Cyan blink, Wi-Fi setup mode; steady
 *            when a device is connected. Blue blink, keypad pairing.
 *            Purple blink, computer pairing. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial.h"
#include "hid.h"
#include "leds.h"
#include "vault.h"
#include "view.h"

#define LED_SIDE        0       /* Under the side key: select */
#define LED_FRONT       (LEDS_COUNT - 1)    /* Under the front key: type. One LED: the same LED. */
#define BRIGHT          64      /* Full light, of 255. The LEDs are bright. */
#define DIM             6       /* After the idle time */
#define BLINK_MS        1000
#define FAST_MS         300
#define FLASH_MS        120     /* A white tick at a push */
#define BLE_TICK_MS     2000    /* One LED: a blue tick with this period while the Bluetooth computer gets the keys */

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct {
    rgb_t color;
    uint16_t blink_ms;      /* 0: steady */
    uint8_t level;          /* Light of 255: the ramp of a hold */
} led_t;

static const rgb_t C_OFF = { 0, 0, 0 };
static const rgb_t C_RED = { 255, 0, 0 };
static const rgb_t C_GREEN = { 0, 255, 0 };
static const rgb_t C_BLUE = { 0, 60, 255 };
static const rgb_t C_CYAN = { 0, 255, 180 };
static const rgb_t C_PURPLE = { 170, 0, 255 };
static const rgb_t C_AMBER = { 255, 110, 0 };
static const rgb_t C_WHITE = { 255, 255, 255 };

/* The color of an entry by its position in the list. */
static const rgb_t s_palette[8] = {
    { 255, 110, 0 },    /* amber */
    { 255, 230, 0 },    /* yellow */
    { 0, 255, 0 },      /* green */
    { 0, 255, 180 },    /* cyan */
    { 0, 60, 255 },     /* blue */
    { 170, 0, 255 },    /* purple */
    { 255, 0, 120 },    /* pink */
    { 255, 255, 255 },  /* white */
};

static led_t s_led[LEDS_COUNT];
static bool s_dim;
static uint32_t s_flash_until;  /* A white tick on the LEDs of s_flash_mask until this time */
static uint8_t s_flash_mask;
static int s_pin_cur, s_pin_len;    /* The last PIN state: a change gives a tick */
static bool s_ble_tick;             /* One LED: the blue tick is on */
static bool s_web_printed;          /* The network is on the serial port */
static bool s_web_connected;

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

/* A line for the user on the serial port. */
static void print(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        serial_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    }
}

static void set(int i, rgb_t color, uint16_t blink_ms, uint8_t level)
{
    s_led[i].color = color;
    s_led[i].blink_ms = blink_ms;
    s_led[i].level = level;
    if (i == LED_FRONT) {
        s_ble_tick = false;     /* A new state of the front LED: view_list turns the tick on again. */
    }
}

static void set_both(rgb_t color, uint16_t blink_ms, uint8_t level)
{
    set(LED_SIDE, color, blink_ms, level);
    set(LED_FRONT, color, blink_ms, level);
}

static void flash(uint8_t mask)
{
    s_flash_mask = mask;
    s_flash_until = now_ms() + FLASH_MS;
}

/* Send the colors of this moment: the blink phase, the flash, the light. */
static void render(void)
{
    uint32_t now = now_ms();
    bool flashing = s_flash_mask != 0 && (int32_t)(s_flash_until - now) > 0;
    int max = s_dim ? DIM : BRIGHT;

    if (!flashing) {
        s_flash_mask = 0;
    }
    for (int i = 0; i < LEDS_COUNT; i++) {
        const led_t *l = &s_led[i];
        rgb_t c = l->color;
        int level = max * l->level / 255;

        if (l->blink_ms > 0 && now % l->blink_ms >= l->blink_ms / 2) {
            c = C_OFF;
        }
        if (s_ble_tick && i == LED_FRONT && now % BLE_TICK_MS < FLASH_MS) {
            c = C_BLUE;
        }
        if (flashing && (s_flash_mask & (1 << i))) {
            c = C_WHITE;
            level = max;
        }
        leds_set(i, c.r * level / 255, c.g * level / 255, c.b * level / 255);
    }
    leds_show();
}

esp_err_t view_init(void)
{
    set_both(C_OFF, 0, 255);
    return leds_init();
}

bool view_poll(void)
{
    render();
    return false;
}

void view_flip(void)
{
}

void view_wake(void)
{
    s_dim = false;
    render();
}

void view_sleep(void)
{
    s_dim = true;
    render();
}

/* The link that gets the keys, on the side LED. */
static void set_link(void)
{
    hid_link_t link = hid_link();

    if (link == HID_LINK_BLE) {
        set(LED_SIDE, C_BLUE, 0, 255);
    } else if (link == HID_LINK_USB) {
        set(LED_SIDE, C_GREEN, 0, 255);
    } else if (blehid_state() != BLEHID_NONE) {
        set(LED_SIDE, C_BLUE, BLINK_MS, 255);
    } else {
        set(LED_SIDE, C_RED, 0, 255);
    }
}

/* A hold of both keys on both LEDs: amber that gets brighter for 2 s,
 * then the color of the mode that a release starts. */
static void set_hold(uint32_t hold_ms)
{
    if (hold_ms < UI_BOTH_MS) {
        set_both(C_AMBER, 0, 40 + 215 * hold_ms / UI_BOTH_MS);
    } else if (hold_ms < UI_PAIR_MS) {
        set_both(C_CYAN, 0, 255);
    } else if (hold_ms < UI_HOST_MS) {
        set_both(C_BLUE, 0, 255);
    } else {
        set_both(C_PURPLE, 0, 255);
    }
}

void view_list(const ui_view_t *v)
{
    int n = vault_count();

    s_web_printed = false;
    s_pin_cur = s_pin_len = 0;
    set_link();
#if LEDS_COUNT == 1
    /* The entry color while a computer gets the keys, else the link. */
    if (n > 0 && hid_ready()) {
        set(LED_FRONT, s_palette[v->sel % 8], 0, 255);
        s_ble_tick = hid_link() == HID_LINK_BLE;
    }
#else
    if (n > 0) {
        set(LED_FRONT, s_palette[v->sel % 8], 0, 255);
    } else {
        set(LED_FRONT, C_OFF, 0, 255);
    }
#endif
    if (v->hold_ms > 0) {
        set_hold(v->hold_ms);
    } else if (v->kp_held >= UI_HINT_MS) {
        /* A keypad key is held: blue that gets brighter until the bind. */
        set(LED_FRONT, C_BLUE, 0, 40 + 215 * v->kp_held / UI_BIND_MS);
    }
    render();
}

/* Locked: the front LED blinks amber, red during the wait after wrong
 * PINs. A push of the side key gives a white tick on the side LED; a
 * digit that goes in or out gives a tick on both. One button: a hold
 * shows steady amber, release for the digit; cyan after 2 s, release for
 * the check. */
void view_pin(const ui_view_t *v)
{
    s_web_printed = false;
    set_link();
    if (v->hold_ms > 0) {
        set(LED_FRONT, v->hold_ms < UI_BOTH_MS ? C_AMBER : C_CYAN, 0, 255);
    } else {
        set(LED_FRONT, vault_wait_s() > 0 ? C_RED : C_AMBER, BLINK_MS, 255);
    }
    if (v->pin_len != s_pin_len) {
        flash((1 << LED_SIDE) | (1 << LED_FRONT));
    } else if (v->pin_cur != s_pin_cur && v->pin_cur > 0) {
        flash(1 << LED_SIDE);
    }
    s_pin_cur = v->pin_cur;
    s_pin_len = v->pin_len;
    render();
}

void view_message(const ui_view_t *v, const char *title, const char *detail, view_tone_t tone)
{
    s_web_printed = false;
    s_pin_cur = s_pin_len = 0;      /* After a PIN check, the next PIN screen gives no tick. */
    if (tone == VIEW_OK) {
        set_both(C_GREEN, 0, 255);
    } else if (tone == VIEW_ERROR) {
        set_both(C_RED, 0, 255);
    } else {
        set_both(C_AMBER, 0, 255);
    }
    if (tone != VIEW_INFO) {
        print("\n%s%s%s\n", title, detail ? ": " : "", detail ? detail : "");
    }
    render();
}

/* Wi-Fi setup mode: cyan blink, steady when a device is connected. The
 * network name and the password go to the serial port one time. */
void view_web(const ui_view_t *v)
{
    bool connected = web_connected();

    if (!s_web_printed) {
        print("\nWi-Fi setup mode: network \"%s\", password %s. Then open http://%s/ (a phone: tap"
              " \"Sign in\").\nA key on the device stops the mode.\n", v->web.ssid, v->web.password, WEB_IP);
        s_web_printed = true;
        s_web_connected = false;
    }
    if (connected && !s_web_connected) {
        print("\nWi-Fi setup mode: a device is connected. Open http://%s/.\n", WEB_IP);
    }
    s_web_connected = connected;
    set_both(C_CYAN, connected ? 0 : BLINK_MS, 255);
    render();
}

void view_pair(const keypad_pair_t *p)
{
    s_web_printed = false;
    switch (p->step) {
    case KEYPAD_PAIR_SEARCHING:
        set_both(C_BLUE, BLINK_MS, 255);
        print("\nPair keypad: turn on the pairing mode of the keypad. A key on the device stops.\n");
        break;
    case KEYPAD_PAIR_CONNECTING:
        set_both(C_BLUE, FAST_MS, 255);
        print("\nPair keypad: connecting to %s.\n", p->name);
        break;
    case KEYPAD_PAIR_PASSKEY:
        set_both(C_BLUE, 0, 255);
        print("\nPair keypad: type %06lu on the keypad, then Enter.\n", (unsigned long)p->passkey);
        break;
    case KEYPAD_PAIR_DONE:
        set_both(C_GREEN, 0, 255);
        print("\nKeypad paired: %s.\n", p->name);
        break;
    case KEYPAD_PAIR_FAILED:
        set_both(C_RED, 0, 255);
        print("\nPairing failed: %s.\n", p->error ? p->error : "unknown error");
        break;
    }
    render();
}

void view_pair_pc(const blehid_pair_t *p)
{
    s_web_printed = false;
    switch (p->step) {
    case BLEHID_PAIR_WAITING:
        set_both(C_PURPLE, BLINK_MS, 255);
        print("\nPair computer: add \"hw-pass\" in the Bluetooth settings of the computer."
              " A key on the device stops.\n");
        break;
    case BLEHID_PAIR_PASSKEY:
        set_both(C_PURPLE, 0, 255);
        print("\nPair computer: type %06lu on the computer, then Enter.\n", (unsigned long)p->passkey);
        break;
    case BLEHID_PAIR_DONE:
        set_both(C_GREEN, 0, 255);
        print("\nComputer paired: %s.\n", p->name);
        break;
    case BLEHID_PAIR_FAILED:
        set_both(C_RED, 0, 255);
        print("\nPairing failed: %s.\n", p->error ? p->error : "unknown error");
        break;
    }
    render();
}
