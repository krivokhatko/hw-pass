#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bluetooth LE keyboard and mouse for a computer (HID over GATT). The
 * device keeps one paired computer and advertises while it is not
 * connected: the computer connects by itself. hid.c sends the keys and
 * the mouse moves. All functions are safe to call from different tasks.
 * A build without Bluetooth (CONFIG_BT_NIMBLE_ENABLED=n) has no computer
 * link: then pairing returns ESP_ERR_NOT_SUPPORTED. */

#define BLEHID_NAME_MAX     19      /* Characters. Fits one line on the LCD. */

typedef enum {
    BLEHID_NONE,        /* No paired computer */
    BLEHID_WAITING,     /* Paired. The device advertises until the computer connects. */
    BLEHID_CONNECTED,   /* The link is encrypted: the keys work. */
} blehid_state_t;

typedef enum {
    BLEHID_PAIR_WAITING,    /* Advertises. The user adds the device on the computer. */
    BLEHID_PAIR_PASSKEY,    /* The user types the passkey on the computer. */
    BLEHID_PAIR_DONE,
    BLEHID_PAIR_FAILED,
} blehid_pair_step_t;

typedef struct {
    blehid_pair_step_t step;
    uint32_t passkey;                   /* BLEHID_PAIR_PASSKEY: 0 to 999999 */
    char name[BLEHID_NAME_MAX + 1];     /* BLEHID_PAIR_DONE: name or address of the computer */
    const char *error;                  /* BLEHID_PAIR_FAILED: short text */
} blehid_pair_t;

/* Load the paired computer and give the HID services to the host. If a
 * computer is paired, start Bluetooth: the device advertises. Call after
 * nvs_flash_init and ble_init, and before keypad_init: the services must
 * be in place before the host starts. */
esp_err_t blehid_init(void);

blehid_state_t blehid_state(void);

/* Copy the name of the paired computer. Empty if there is none. */
void blehid_name(char *buf, size_t size);

/* Advertise as discoverable and accept one pairing. The screen shows the
 * passkey (ui.c). The new computer replaces the old one. Starts Bluetooth
 * if it does not run. */
esp_err_t blehid_pair_start(void);

/* Get the state of the pairing. */
void blehid_pair_status(blehid_pair_t *pair);

/* Stop the pairing if it is not done. The old computer stays paired. */
void blehid_pair_stop(void);

/* Remove the paired computer and its bonding keys. ESP_ERR_NOT_FOUND if
 * there is none. */
esp_err_t blehid_unpair(void);

/* The paired computer is connected, the link is encrypted, and the
 * computer takes key reports. */
bool blehid_ready(void);

/* Caps Lock state from the computer. */
bool blehid_caps_lock(void);

/* Send a key report: modifiers and up to 6 keys that are down (HID usage
 * IDs, page 0x07). Waits up to 200 ms for a free buffer. ESP_ERR_INVALID_STATE
 * if the computer is not ready. */
esp_err_t blehid_send_keys(uint8_t mod, const uint8_t keys[6]);

/* Send a mouse move. Does not wait. */
esp_err_t blehid_send_mouse(int8_t dx, int8_t dy);

#ifdef __cplusplus
}
#endif
