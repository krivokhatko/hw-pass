/* Encrypted backup of the entries, in the file format of
 * "openssl enc -a -aes-256-cbc -pbkdf2 -md sha512":
 * - "Salted__", an 8-byte random salt, then the AES-256-CBC cipher text.
 * - PBKDF2-HMAC-SHA512 of the passphrase and the salt gives the key and the IV.
 * - PKCS#7 padding. Base64 text in lines of 64 characters. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "sha/sha_core.h"
#include "rng.h"
#include "vault.h"
#include "backup.h"

#define MAGIC       "Salted__"
#define HEAD_LEN    16      /* MAGIC and the salt */
#define SALT_LEN    8
#define KEY_LEN     32
#define BLOCK       16      /* AES block, also the IV length */
#define HASH_LEN    64      /* SHA-512 digest */
#define SHA_BLOCK   128     /* SHA-512 block */
#define LINE_BYTES  48      /* Bytes in one base64 line of 64 characters */
#define YIELD_EVERY 2000    /* PBKDF2 iterations between task switches */

static const char *TAG = "backup";

/* SHA-512 of one block with the hardware, from state (NULL: the initial
 * state). Call between esp_sha_acquire_hardware and
 * esp_sha_release_hardware. The state has the byte order of a digest. */
static void sha_block(const uint32_t *state, const uint32_t *block, uint32_t *out)
{
    if (state) {
        esp_sha_write_digest_state(SHA2_512, (void *)state);
    }
    esp_sha_block(SHA2_512, block, state == NULL);
    esp_sha_read_digest_state(SHA2_512, out);   /* Waits until the block is done. */
}

/* PBKDF2-HMAC-SHA512 (RFC 8018), one block of 64 bytes: enough for the key
 * and the IV. mbedTLS makes U1. The hardware makes the other U values from
 * the saved states after the ipad and opad blocks: 2 blocks for each
 * iteration. mbedtls_md_hmac needs 4 blocks and takes the engine for each
 * one: 5 times slower. Other tasks can run between the parts. */
static esp_err_t derive(const char *pass, const uint8_t *salt, uint8_t out[HASH_LEN])
{
    static const uint8_t block_1[4] = { 0, 0, 0, 1 };
    const size_t pass_len = strlen(pass);   /* Not longer than one block: no hash of the key */
    uint32_t pad[SHA_BLOCK / 4], istate[HASH_LEN / 4], ostate[HASH_LEN / 4];
    uint32_t msg[SHA_BLOCK / 4], u[HASH_LEN / 4];
    uint8_t *p = (uint8_t *)pad, *m = (uint8_t *)msg;
    mbedtls_md_context_t ctx;
    int rc;

    mbedtls_md_init(&ctx);
    rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), 1);
    if (rc == 0) {
        rc = mbedtls_md_hmac_starts(&ctx, (const uint8_t *)pass, pass_len);
    }
    if (rc == 0) {
        rc = mbedtls_md_hmac_update(&ctx, salt, SALT_LEN);
    }
    if (rc == 0) {
        rc = mbedtls_md_hmac_update(&ctx, block_1, sizeof(block_1));
    }
    if (rc == 0) {
        rc = mbedtls_md_hmac_finish(&ctx, (uint8_t *)u);
    }
    mbedtls_md_free(&ctx);
    if (rc != 0) {
        return ESP_FAIL;
    }
    memcpy(out, u, HASH_LEN);

    /* Message block of U(i): U(i-1), 0x80, zeros, the length in bits of
     * the key block and the 64 bytes. The same for the outer hash. */
    memset(msg, 0, sizeof(msg));
    m[HASH_LEN] = 0x80;
    m[SHA_BLOCK - 2] = ((SHA_BLOCK + HASH_LEN) * 8) >> 8;
    m[SHA_BLOCK - 1] = ((SHA_BLOCK + HASH_LEN) * 8) & 0xFF;

    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_512);
    memset(pad, 0, sizeof(pad));
    memcpy(pad, pass, pass_len);
    for (int j = 0; j < SHA_BLOCK; j++) {
        p[j] ^= 0x36;
    }
    sha_block(NULL, pad, istate);
    for (int j = 0; j < SHA_BLOCK; j++) {
        p[j] ^= 0x36 ^ 0x5C;
    }
    sha_block(NULL, pad, ostate);
    vault_zero(pad, sizeof(pad));

    for (int i = 1; i < BACKUP_ITERATIONS; i++) {
        memcpy(msg, u, HASH_LEN);
        sha_block(istate, msg, u);
        memcpy(msg, u, HASH_LEN);
        sha_block(ostate, msg, u);
        for (int j = 0; j < HASH_LEN; j++) {
            out[j] ^= ((uint8_t *)u)[j];
        }
        if (i % YIELD_EVERY == 0) {
            esp_sha_release_hardware();
            vTaskDelay(1);
            esp_sha_acquire_hardware();
            esp_sha_set_mode(SHA2_512);
        }
    }
    esp_sha_release_hardware();
    vault_zero(istate, sizeof(istate));
    vault_zero(ostate, sizeof(ostate));
    vault_zero(msg, sizeof(msg));
    vault_zero(u, sizeof(u));
    return ESP_OK;
}

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
    err = derive(pass, file + SALT_LEN, key_iv);
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
