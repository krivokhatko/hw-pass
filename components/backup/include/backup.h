#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKUP_PASS_MIN     12      /* Characters of the passphrase */
#define BACKUP_PASS_MAX     127
#define BACKUP_ITERATIONS   210000  /* PBKDF2. Part of the file format. */

/* Make an encrypted backup of all entries. This command decrypts it:
 * openssl enc -d -a -aes-256-cbc -pbkdf2 -md sha512 -iter 210000 -in <file>
 * The plain text has one "label,password" line for each entry.
 *
 * pass: printable ASCII, BACKUP_PASS_MIN to BACKUP_PASS_MAX characters.
 * On success, *text is base64 text in lines of 64 characters. Free it
 * with free(). Takes some seconds. */
esp_err_t backup_export(const char *pass, char **text);

#ifdef __cplusplus
}
#endif
