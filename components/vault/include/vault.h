#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VAULT_MAX           32      /* Number of entries */
#define VAULT_LABEL_MAX     19      /* Characters. Fits one line on the LCD. */
#define VAULT_SECRET_MAX    127     /* Characters */
#define VAULT_PIN_MIN       4       /* Digits */
#define VAULT_PIN_MAX       12

/* Entries are 0-based. Labels and secrets are printable ASCII (0x20..0x7E).
 * All functions are safe to call from different tasks.
 *
 * A PIN encrypts the entries in flash. Without a PIN, they are plain
 * text. While the device is locked, RAM has no entries: the functions
 * below return ESP_ERR_NOT_ALLOWED, vault_count returns 0, and
 * vault_find_key returns -1. vault_wipe works also while locked: it is
 * the way out after a lost PIN. */

/* Open the "vault" NVS partition and load the entries. With a PIN, the
 * device starts locked. After an error, the vault is empty and the other
 * functions are still safe to call. */
esp_err_t vault_init(void);

int vault_count(void);

/* The value changes after each change of the entries, a lock, and an unlock. */
uint32_t vault_generation(void);

/* Copy the label or the secret of entry i. Buffer sizes must be at
 * least VAULT_LABEL_MAX + 1 and VAULT_SECRET_MAX + 1. */
esp_err_t vault_label(int i, char *buf, size_t size);
esp_err_t vault_secret(int i, char *buf, size_t size);

esp_err_t vault_add(const char *label, const char *secret);

/* Add n entries with one write. Adds all or nothing. */
esp_err_t vault_add_many(const char *const labels[], const char *const secrets[], int n);
esp_err_t vault_set_label(int i, const char *label);
esp_err_t vault_set_secret(int i, const char *secret);
esp_err_t vault_delete(int i);

/* Move entry `from` to position `to`. The entries between shift by one. */
esp_err_t vault_move(int from, int to);

/* Keypad key of entry i: a HID key code. 0: no key, or no entry i. */
uint8_t vault_key(int i);

/* Return the entry with this key, or -1. */
int vault_find_key(uint8_t key);

/* Give entry i this key. Another entry with this key loses it. Key 0
 * removes the key of entry i. */
esp_err_t vault_set_key(int i, uint8_t key);

/* Erase the partition: the entries, the PIN, and the deleted data in
 * flash. After it, the device is not locked. */
esp_err_t vault_wipe(void);

/* ---- PIN ---- */

/* A PIN is set: the entries in flash are encrypted. */
bool vault_has_pin(void);

/* A PIN is set, and the entries are not in RAM. */
bool vault_locked(void);

/* Return ESP_OK if s has VAULT_PIN_MIN to VAULT_PIN_MAX digits. */
esp_err_t vault_check_pin(const char *s);

/* Set a PIN, or change it. The entries get a new key. Not while locked.
 * Takes about 1 s. */
esp_err_t vault_set_pin(const char *pin);

/* Remove the PIN: the entries become plain text. Not while locked. */
esp_err_t vault_clear_pin(void);

/* Decrypt the entries into RAM. Takes about 1 s. Returns
 * ESP_ERR_INVALID_ARG after a wrong PIN, ESP_ERR_TIMEOUT while
 * vault_wait_s is not 0, and ESP_ERR_INVALID_STATE without a PIN. */
esp_err_t vault_unlock(const char *pin);

/* Remove the entries and the key from RAM. Does nothing without a PIN. */
void vault_lock(void);

/* After 3 wrong PINs in a row, each try waits two times longer than the
 * last one, up to 1 h. A restart does not reset the wait. Returns the
 * seconds until the next try, 0 if a try can go now. */
int vault_wait_s(void);

/* Wrong PINs in a row. 0 after a correct PIN. */
int vault_fails(void);

/* The chip has an HMAC key in eFuse (purpose HMAC_UP). The PIN key uses
 * it: then the flash alone cannot give the entries. */
bool vault_efuse_key(void);

/* The PIN key of the stored entries uses the eFuse key. False if the key
 * was burned after the PIN was set: set the PIN again. */
bool vault_pin_bound(void);

/* Hold the lock over more than one call. Between vault_hold and
 * vault_release, other tasks cannot change the entries. Keep it short. */
void vault_hold(void);
void vault_release(void);

/* Return ESP_OK if s has 1..max printable ASCII characters. */
esp_err_t vault_check_text(const char *s, size_t max);

/* Set memory to zero. The compiler cannot remove this call. */
void vault_zero(void *p, size_t n);

#ifdef __cplusplus
}
#endif
