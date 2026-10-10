#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VAULT_MAX           32      /* Number of entries */
#define VAULT_LABEL_MAX     19      /* Characters. Fits one line on the LCD. */
#define VAULT_SECRET_MAX    127     /* Characters */

/* Entries are 0-based. Labels and secrets are printable ASCII (0x20..0x7E).
 * All functions are safe to call from different tasks. */

/* Open the "vault" NVS partition and load the entries. After an error,
 * the vault is empty and the other functions are still safe to call. */
esp_err_t vault_init(void);

int vault_count(void);

/* The value changes after each change of the entries. */
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

/* Erase the partition. This also removes the deleted data from flash. */
esp_err_t vault_wipe(void);

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
