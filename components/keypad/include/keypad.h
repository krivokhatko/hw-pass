#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bluetooth LE keypad: a HID keyboard paired with the device. The UI
 * reads the key changes. All functions are safe to call from different
 * tasks. A build without Bluetooth (CONFIG_BT_NIMBLE_ENABLED=n) has no
 * keypad: then pairing returns ESP_ERR_NOT_SUPPORTED. */

#define KEYPAD_NAME_MAX     19      /* Characters. Fits one line on the LCD. */
#define KEYPAD_KEY_NAME     8       /* Buffer size for keypad_key_name */

typedef enum {
    KEYPAD_NONE,        /* No paired keypad */
    KEYPAD_WAITING,     /* Paired. Waits until the keypad connects. */
    KEYPAD_CONNECTED,   /* The keys work. */
} keypad_state_t;

typedef enum {
    KEYPAD_PAIR_SEARCHING,  /* Looks for a keypad in pairing mode. */
    KEYPAD_PAIR_CONNECTING,
    KEYPAD_PAIR_PASSKEY,    /* The user types the passkey on the keypad. */
    KEYPAD_PAIR_DONE,
    KEYPAD_PAIR_FAILED,
} keypad_pair_step_t;

typedef struct {
    keypad_pair_step_t step;
    uint32_t passkey;                   /* KEYPAD_PAIR_PASSKEY: 0 to 999999 */
    char name[KEYPAD_NAME_MAX + 1];     /* From CONNECTING: name or address */
    const char *error;                  /* KEYPAD_PAIR_FAILED: short text */
} keypad_pair_t;

/* A key went down or up. Key 0 is a cancel: all keys are up, but they
 * were not pushed. The link is lost or events are lost. */
typedef struct {
    uint8_t key;        /* HID key code (usage ID, page 0x07), or 0 */
    bool down;
    uint32_t ms;        /* Time of the change, in ms of the tick count */
} keypad_event_t;

/* Load the paired keypad. If there is one, start Bluetooth: the device
 * connects when the keypad is on. Call after nvs_flash_init. */
esp_err_t keypad_init(void);

keypad_state_t keypad_state(void);

/* Copy the name of the paired keypad. Empty if there is none. */
void keypad_name(char *buf, size_t size);

/* Look for a keypad in pairing mode, connect, and pair. When the pairing
 * is done, the new keypad replaces the old one. Starts Bluetooth if it
 * does not run. Only a push of the buttons may start a pairing. */
esp_err_t keypad_pair_start(void);

/* Get the state of the pairing. */
void keypad_pair_status(keypad_pair_t *pair);

/* Stop the pairing if it is not done. The old keypad stays paired. */
void keypad_pair_stop(void);

/* Remove the paired keypad. ESP_ERR_NOT_FOUND if there is none. */
esp_err_t keypad_unpair(void);

/* Get the next key change. Return false if there is none. */
bool keypad_event(keypad_event_t *ev);

/* Remove the key changes that the UI did not read. */
void keypad_flush(void);

/* Short name of a key for the screen: "5", "Enter", "NumLk". */
void keypad_key_name(uint8_t key, char buf[KEYPAD_KEY_NAME]);

#ifdef __cplusplus
}
#endif
