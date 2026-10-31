/* PBKDF2-HMAC-SHA512 on the SHA hardware. mbedTLS makes U1. The hardware
 * makes the other U values from the saved states after the ipad and opad
 * blocks: 2 blocks for each iteration. mbedtls_md_hmac needs 4 blocks and
 * takes the engine for each one: 5 times slower. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "sha/sha_core.h"
#include "kdf.h"

#define SHA_BLOCK   128     /* SHA-512 block */
#define YIELD_EVERY 5000    /* Iterations between task switches: about 90 ms */

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

esp_err_t kdf_pbkdf2_sha512(const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t out[KDF_HASH_LEN])
{
    static const uint8_t block_1[4] = { 0, 0, 0, 1 };
    uint32_t pad[SHA_BLOCK / 4], istate[KDF_HASH_LEN / 4], ostate[KDF_HASH_LEN / 4];
    uint32_t msg[SHA_BLOCK / 4], u[KDF_HASH_LEN / 4];
    uint8_t *p = (uint8_t *)pad, *m = (uint8_t *)msg;
    mbedtls_md_context_t ctx;
    int rc;

    if (pass_len > KDF_PASS_MAX || iterations == 0) {
        return ESP_ERR_INVALID_ARG;     /* A longer password gets hashed first: not done here. */
    }
    mbedtls_md_init(&ctx);
    rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), 1);
    if (rc == 0) {
        rc = mbedtls_md_hmac_starts(&ctx, pass, pass_len);
    }
    if (rc == 0) {
        rc = mbedtls_md_hmac_update(&ctx, salt, salt_len);
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
    memcpy(out, u, KDF_HASH_LEN);

    /* Message block of U(i): U(i-1), 0x80, zeros, the length in bits of
     * the key block and the 64 bytes. The same for the outer hash. */
    memset(msg, 0, sizeof(msg));
    m[KDF_HASH_LEN] = 0x80;
    m[SHA_BLOCK - 2] = ((SHA_BLOCK + KDF_HASH_LEN) * 8) >> 8;
    m[SHA_BLOCK - 1] = ((SHA_BLOCK + KDF_HASH_LEN) * 8) & 0xFF;

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
    mbedtls_platform_zeroize(pad, sizeof(pad));

    for (uint32_t i = 1; i < iterations; i++) {
        memcpy(msg, u, KDF_HASH_LEN);
        sha_block(istate, msg, u);
        memcpy(msg, u, KDF_HASH_LEN);
        sha_block(ostate, msg, u);
        for (int j = 0; j < KDF_HASH_LEN; j++) {
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
    mbedtls_platform_zeroize(istate, sizeof(istate));
    mbedtls_platform_zeroize(ostate, sizeof(ostate));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    mbedtls_platform_zeroize(u, sizeof(u));
    return ESP_OK;
}
