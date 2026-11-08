/* PBKDF2-HMAC-SHA512 on the SHA hardware. mbedTLS makes U1. The hardware
 * makes the other U values; mbedtls_md_hmac would take the engine for
 * each of its 4 blocks: much slower.
 * - A chip whose engine takes a saved state (ESP32-S3): the states after
 *   the ipad and opad blocks are saved, and each iteration hashes 2
 *   blocks (sha_core.h).
 * - The ESP32: the engine starts from the first block each time, thus
 *   each iteration hashes 4 blocks (sha_parallel_engine.h). Its SHA-512
 *   engine is also slower (about 17 us for a block): 50000 iterations
 *   take about 5 s, against 1 s on the ESP32-S3. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "soc/soc_caps.h"
#include "kdf.h"

#if SOC_SHA_SUPPORT_RESUME
#include "sha/sha_core.h"
#else
#include "sha/sha_parallel_engine.h"
#endif

#define SHA_BLOCK   128     /* SHA-512 block */
#define SHA_STATE   (KDF_HASH_LEN / 4)  /* State words */
#define YIELD_EVERY 5000    /* Iterations between task switches: about 90 ms */

/* The HMAC key blocks: the password with 0x36 (inner hash) and with 0x5C
 * (outer hash), and the states after them where the engine takes them. */
typedef struct {
    uint32_t ipad[SHA_BLOCK / 4];
    uint32_t opad[SHA_BLOCK / 4];
#if SOC_SHA_SUPPORT_RESUME
    uint32_t istate[SHA_STATE];
    uint32_t ostate[SHA_STATE];
#endif
} hmac_t;

#if SOC_SHA_SUPPORT_RESUME

static void engine_take(void)
{
    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_512);
}

static void engine_give(void)
{
    esp_sha_release_hardware();
}

/* SHA-512 of one block with the hardware, from state (NULL: the initial
 * state). The state has the byte order of a digest. */
static void sha_block(const uint32_t *state, const uint32_t *block, uint32_t *out)
{
    if (state) {
        esp_sha_write_digest_state(SHA2_512, (void *)state);
    }
    esp_sha_block(SHA2_512, block, state == NULL);
    esp_sha_read_digest_state(SHA2_512, out);   /* Waits until the block is done. */
}

static void hmac_setup(hmac_t *h)
{
    sha_block(NULL, h->ipad, h->istate);
    sha_block(NULL, h->opad, h->ostate);
}

/* U(i) from U(i-1): the inner hash from the ipad state, then the outer
 * hash from the opad state. msg is the padded message block; u holds
 * U(i-1) and gets U(i). */
static void hmac_step(const hmac_t *h, uint32_t *msg, uint32_t *u)
{
    memcpy(msg, u, KDF_HASH_LEN);
    sha_block(h->istate, msg, u);
    memcpy(msg, u, KDF_HASH_LEN);
    sha_block(h->ostate, msg, u);
}

#else   /* The ESP32 */

static void engine_take(void)
{
    esp_sha_lock_engine(SHA2_512);
}

static void engine_give(void)
{
    esp_sha_unlock_engine(SHA2_512);
}

static void hmac_setup(hmac_t *h)
{
}

/* The engine gives the state as 8 words of 64 bits in the order of the
 * machine (as mbedTLS keeps it). The digest is these words, big endian. */
static void digest_bytes(const uint64_t *state, uint32_t *out)
{
    uint8_t *p = (uint8_t *)out;

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            p[i * 8 + j] = (uint8_t)(state[i] >> (56 - 8 * j));
        }
    }
}

/* U(i) from U(i-1): 2 blocks for the inner hash, 2 for the outer. The
 * padding after the 64 bytes of msg is the same for both hashes. */
static void hmac_step(const hmac_t *h, uint32_t *msg, uint32_t *u)
{
    uint64_t state[8];

    memcpy(msg, u, KDF_HASH_LEN);
    esp_sha_block(SHA2_512, h->ipad, true);
    esp_sha_block(SHA2_512, msg, false);
    esp_sha_read_digest_state(SHA2_512, state);
    digest_bytes(state, msg);
    esp_sha_block(SHA2_512, h->opad, true);
    esp_sha_block(SHA2_512, msg, false);
    esp_sha_read_digest_state(SHA2_512, state);
    digest_bytes(state, u);
}

#endif  /* SOC_SHA_SUPPORT_RESUME */

esp_err_t kdf_pbkdf2_sha512(const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t out[KDF_HASH_LEN])
{
    static const uint8_t block_1[4] = { 0, 0, 0, 1 };
    hmac_t h;
    uint32_t msg[SHA_BLOCK / 4], u[SHA_STATE];
    uint8_t *m = (uint8_t *)msg, *ip = (uint8_t *)h.ipad, *op = (uint8_t *)h.opad;
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

    memset(&h, 0, sizeof(h));
    memcpy(h.ipad, pass, pass_len);
    memcpy(h.opad, pass, pass_len);
    for (int j = 0; j < SHA_BLOCK; j++) {
        ip[j] ^= 0x36;
        op[j] ^= 0x5C;
    }

    engine_take();
    hmac_setup(&h);
    for (uint32_t i = 1; i < iterations; i++) {
        hmac_step(&h, msg, u);
        for (int j = 0; j < KDF_HASH_LEN; j++) {
            out[j] ^= ((uint8_t *)u)[j];
        }
        if (i % YIELD_EVERY == 0) {
            engine_give();
            vTaskDelay(1);
            engine_take();
        }
    }
    engine_give();
    mbedtls_platform_zeroize(&h, sizeof(h));
    mbedtls_platform_zeroize(msg, sizeof(msg));
    mbedtls_platform_zeroize(u, sizeof(u));
    return ESP_OK;
}
