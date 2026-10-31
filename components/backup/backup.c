/* Encrypted backup of the entries, in the file format of
 * "openssl enc -a -aes-256-cbc -pbkdf2 -md sha512":
 * - "Salted__", an 8-byte random salt, then the AES-256-CBC cipher text.
 * - PBKDF2-HMAC-SHA512 of the passphrase and the salt gives the key and the IV.
 * - PKCS#7 padding. Base64 text in lines of 64 characters. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "kdf.h"
#include "rng.h"
#include "vault.h"
#include "backup.h"

#define MAGIC       "Salted__"
#define HEAD_LEN    16      /* MAGIC and the salt */
#define SALT_LEN    8
#define KEY_LEN     32
#define BLOCK       16      /* AES block, also the IV length */
#define HASH_LEN    KDF_HASH_LEN    /* Key and IV */
#define LINE_BYTES  48      /* Bytes in one base64 line of 64 characters */

static const char *TAG = "backup";

/* One "label,password" line for each entry. Return the length. */
static size_t plain_text(char *buf, size_t size)
{
    char label[VAULT_LABEL_MAX + 1], secret[VAULT_SECRET_MAX + 1];
    size_t n = 0;

    vault_hold();   /* All entries from one state */
    for (int i = 0; vault_label(i, label, sizeof(label)) == ESP_OK &&
                    vault_secret(i, secret, sizeof(secret)) == ESP_OK; i++) {
        n += snprintf(buf + n, size - n, "%s,%s\n", label, secret);
    }
    vault_release();
    vault_zero(secret, sizeof(secret));
    return n;
}

/* Encrypt n bytes of plain text (with space for the padding) to file. */
static esp_err_t encrypt(const char *pass, uint8_t *plain, size_t n, uint8_t *file)
{
    uint8_t pad = BLOCK - n % BLOCK, key_iv[HASH_LEN];
    mbedtls_aes_context aes;
    esp_err_t err;

    memset(plain + n, pad, pad);
    n += pad;
    memcpy(file, MAGIC, SALT_LEN);
    rng_bytes(file + SALT_LEN, SALT_LEN);
    err = kdf_pbkdf2_sha512((const uint8_t *)pass, strlen(pass), file + SALT_LEN, SALT_LEN, BACKUP_ITERATIONS, key_iv);
    if (err == ESP_OK) {
        mbedtls_aes_init(&aes);
        if (mbedtls_aes_setkey_enc(&aes, key_iv, KEY_LEN * 8) != 0 ||
            mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, n, key_iv + KEY_LEN, plain, file + HEAD_LEN) != 0) {
            err = ESP_FAIL;
        }
        mbedtls_aes_free(&aes);
    }
    vault_zero(key_iv, sizeof(key_iv));
    return err;
}

/* Base64 in lines of 64 characters, each with a line feed. */
static char *base64_lines(const uint8_t *data, size_t len)
{
    size_t size = (len + LINE_BYTES - 1) / LINE_BYTES * 65 + 1, pos = 0, olen;
    char *text = malloc(size);

    if (!text) {
        return NULL;
    }
    for (size_t i = 0; i < len; i += LINE_BYTES) {
        size_t part = len - i < LINE_BYTES ? len - i : LINE_BYTES;

        if (mbedtls_base64_encode((uint8_t *)text + pos, size - pos, &olen, data + i, part) != 0) {
            free(text);
            return NULL;
        }
        pos += olen;
        text[pos++] = '\n';
    }
    text[pos] = '\0';
    return text;
}

esp_err_t backup_export(const char *pass, char **text)
{
    const size_t plain_max = VAULT_MAX * (VAULT_LABEL_MAX + VAULT_SECRET_MAX + 2) + 1 + BLOCK;
    uint8_t *plain, *file;
    esp_err_t err;
    size_t n;

    *text = NULL;
    err = vault_check_text(pass, BACKUP_PASS_MAX);
    if (err == ESP_OK && strlen(pass) < BACKUP_PASS_MIN) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (vault_locked()) {
        return ESP_ERR_NOT_ALLOWED;     /* An empty backup must not look complete. */
    }
    plain = malloc(plain_max);
    file = malloc(HEAD_LEN + plain_max);
    if (!plain || !file) {
        free(plain);
        free(file);
        return ESP_ERR_NO_MEM;
    }
    n = plain_text((char *)plain, plain_max - BLOCK);
    err = encrypt(pass, plain, n, file);
    vault_zero(plain, plain_max);
    free(plain);
    if (err == ESP_OK) {
        *text = base64_lines(file, HEAD_LEN + n + BLOCK - n % BLOCK);
        err = *text ? ESP_OK : ESP_ERR_NO_MEM;
    }
    free(file);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "export: %s", esp_err_to_name(err));
    }
    return err;
}
