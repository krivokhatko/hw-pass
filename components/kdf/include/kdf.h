#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDF_HASH_LEN    64      /* SHA-512 digest */
#define KDF_PASS_MAX    128     /* Bytes of the password: one SHA-512 block */

/* PBKDF2-HMAC-SHA512 (RFC 8018), the first block of 64 bytes. The SHA
 * hardware does the work, and other tasks run between the parts. The
 * time grows with the iterations: about 18 us for each one. Returns
 * ESP_ERR_INVALID_ARG if the password is longer than KDF_PASS_MAX. */
esp_err_t kdf_pbkdf2_sha512(const uint8_t *pass, size_t pass_len, const uint8_t *salt, size_t salt_len,
                            uint32_t iterations, uint8_t out[KDF_HASH_LEN]);

#ifdef __cplusplus
}
#endif
