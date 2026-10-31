/* Password entries. RAM holds a copy. Each change writes all entries
 * as one NVS blob, thus a power loss keeps the old or the new set.
 * Version 1 had no keypad key. The first change after an update writes
 * version 2.
 *
 * With a PIN, the blob is encrypted ("sealed"): AES-256-GCM with a new
 * nonce at each write, and the GCM tag checks the PIN. The key comes from
 * PBKDF2-HMAC-SHA512 of the PIN and a salt. With an HMAC key in eFuse,
 * the HMAC of the PIN replaces the PIN in the PBKDF2: then the key
 * exists only on this chip, and a copy of the flash cannot give the
 * entries. The salt and the other parameters are in the sealed blob, in
 * front of the data: one write changes the PIN, the key, and the data. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "kdf.h"
#include "vault.h"

#define PARTITION   "vault"
#define NAMESPACE   "vault"
#define KEY_PLAIN   "entries"   /* Without a PIN */
#define KEY_SEALED  "sealed"    /* With a PIN */
#define KEY_FAILS   "fails"
#define VERSION     2
#define V1_ENTRY    (VAULT_LABEL_MAX + 1 + VAULT_SECRET_MAX + 1)

#define SEAL_VERSION    1
#define SALT_LEN        16
#define NONCE_LEN       12
#define TAG_LEN         16
#define AES_KEY_LEN     32
#define ITERATIONS      50000   /* PBKDF2: about 1 s on the SHA hardware */
#define ITERATIONS_MAX  2000000 /* Limit for the stored value */
#define FAILS_FREE      3       /* Wrong PINs before the first wait */
#define WAIT_MIN_S      10
#define WAIT_MAX_S      3600

typedef struct {
    char label[VAULT_LABEL_MAX + 1];
    char secret[VAULT_SECRET_MAX + 1];
    uint8_t key;    /* Keypad key, 0: none. Version 2. */
} entry_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    uint8_t reserved[2];
    entry_t entry[VAULT_MAX];
} store_t;

/* Parameters of the PIN key. Also the additional data of the GCM. */
typedef struct {
    uint8_t version;
    uint8_t bound;          /* The eFuse HMAC key is part of the key. */
    uint8_t reserved[2];
    uint32_t iterations;
    uint8_t salt[SALT_LEN];
} params_t;

typedef struct {
    params_t params;
    uint8_t nonce[NONCE_LEN];
    uint8_t tag[TAG_LEN];
    uint8_t data[sizeof(store_t)];  /* Encrypted store_t, store_size bytes */
} sealed_t;

#define SEALED_HEAD     offsetof(sealed_t, data)

static const char *TAG = "vault";
static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static store_t s_store;
static uint32_t s_generation;
static bool s_has_pin;
static bool s_locked;
static params_t s_params;
static uint8_t s_key[AES_KEY_LEN];
static uint8_t s_fails;
static uint32_t s_allow_ms;     /* Tick time when the next unlock try can go */

void vault_zero(void *p, size_t n)
{
    volatile uint8_t *v = p;

    while (n--) {
        *v++ = 0;
    }
}

esp_err_t vault_check_text(const char *s, size_t max)
{
    size_t n = strnlen(s, max + 1);

    if (n == 0 || n > max) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t vault_check_pin(const char *s)
{
    size_t n = strnlen(s, VAULT_PIN_MAX + 1);

    if (n < VAULT_PIN_MIN || n > VAULT_PIN_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

static size_t store_size(int count)
{
    return offsetof(store_t, entry) + count * sizeof(entry_t);
}

/* ---- Keys ---- */

/* The eFuse key block with the purpose HMAC_UP, as an HMAC key ID, or -1. */
static int efuse_key_id(void)
{
    esp_efuse_block_t block;

    if (esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block)) {
        return HMAC_KEY0 + (block - EFUSE_BLK_KEY0);
    }
    return -1;
}

bool vault_efuse_key(void)
{
    return efuse_key_id() >= 0;
}

/* The AES key from the PIN and the parameters. */
static esp_err_t derive(const char *pin, const params_t *p, uint8_t key[AES_KEY_LEN])
{
    uint8_t secret[32], hash[KDF_HASH_LEN];
    const uint8_t *pass = (const uint8_t *)pin;
    size_t pass_len = strlen(pin);
    esp_err_t err;

    if (p->bound) {
        int id = efuse_key_id();

        if (id < 0) {
            ESP_LOGE(TAG, "the entries need the eFuse key, but the chip has none");
            return ESP_ERR_NOT_FOUND;
        }
        err = esp_hmac_calculate(id, pin, pass_len, secret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hmac: %s", esp_err_to_name(err));
            return err;
        }
        pass = secret;
        pass_len = sizeof(secret);
    }
    err = kdf_pbkdf2_sha512(pass, pass_len, p->salt, SALT_LEN, p->iterations, hash);
    memcpy(key, hash, AES_KEY_LEN);
    vault_zero(secret, sizeof(secret));
    vault_zero(hash, sizeof(hash));
    return err;
}

/* AES-256-GCM of n bytes. The parameters are the additional data. For a
 * decrypt, ESP_ERR_INVALID_ARG means a wrong key or changed data. */
static esp_err_t gcm(bool encrypt, const uint8_t *key, const params_t *p, const uint8_t *nonce,
                     const uint8_t *in, uint8_t *out, size_t n, uint8_t *tag)
{
    mbedtls_gcm_context ctx;
    int rc;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_LEN * 8);
    if (rc == 0 && encrypt) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, n, nonce, NONCE_LEN,
                                       (const uint8_t *)p, sizeof(*p), in, out, TAG_LEN, tag);
    } else if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx, n, nonce, NONCE_LEN, (const uint8_t *)p, sizeof(*p),
                                      tag, TAG_LEN, in, out);
    }
    mbedtls_gcm_free(&ctx);
    if (rc == MBEDTLS_ERR_GCM_AUTH_FAILED) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gcm: -0x%04x", -rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- Flash ---- */

/* Version 1 entries have no key. Move them to their version 2 positions,
 * the last first: a version 2 entry is longer. */
static bool from_v1(size_t size)
{
    uint8_t *p = (uint8_t *)s_store.entry;

    if (size != offsetof(store_t, entry) + s_store.count * V1_ENTRY) {
        return false;
    }
    for (int i = s_store.count - 1; i >= 0; i--) {
        memmove(&s_store.entry[i], p + i * V1_ENTRY, V1_ENTRY);
        s_store.entry[i].key = 0;
    }
    return true;
}

/* Check s_store after a read of size bytes. */
static esp_err_t check_store(size_t size)
{
    if (s_store.version == 1 && s_store.count <= VAULT_MAX && from_v1(size)) {
        return ESP_OK;
    }
    if (s_store.version != VERSION || s_store.count > VAULT_MAX || size != store_size(s_store.count)) {
        ESP_LOGE(TAG, "unknown data: version %u, %u entries, %u bytes",
                 s_store.version, s_store.count, (unsigned)size);
        vault_zero(&s_store, sizeof(s_store));
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t load_plain(void)
{
    size_t size = sizeof(s_store);
    esp_err_t err;

    vault_zero(&s_store, sizeof(s_store));
    err = nvs_get_blob(s_nvs, KEY_PLAIN, &s_store, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read");
    return check_store(size);
}

/* Read the sealed blob. On success, *out has it and *size its length. */
static esp_err_t read_sealed(sealed_t **out, size_t *size)
{
    sealed_t *s = malloc(sizeof(sealed_t));
    esp_err_t err;

    *out = NULL;
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    *size = sizeof(sealed_t);
    err = nvs_get_blob(s_nvs, KEY_SEALED, s, size);
    if (err == ESP_OK && (*size < SEALED_HEAD + offsetof(store_t, entry) || s->params.version != SEAL_VERSION ||
                          s->params.iterations == 0 || s->params.iterations > ITERATIONS_MAX)) {
        ESP_LOGE(TAG, "unknown sealed data: version %u, %u bytes", s->params.version, (unsigned)*size);
        err = ESP_ERR_INVALID_VERSION;
    }
    if (err != ESP_OK) {
        free(s);
        return err;
    }
    *out = s;
    return ESP_OK;
}

/* Read the sealed blob and decrypt it with key into s_store.
 * ESP_ERR_INVALID_ARG: the key is wrong. */
static esp_err_t load_sealed(const uint8_t *key)
{
    sealed_t *s;
    size_t size;
    esp_err_t err;

    vault_zero(&s_store, sizeof(s_store));
    err = read_sealed(&s, &size);
    if (err == ESP_OK) {
        size -= SEALED_HEAD;
        err = gcm(false, key, &s->params, s->nonce, s->data, (uint8_t *)&s_store, size, s->tag);
        if (err == ESP_OK) {
            err = check_store(size);
        } else {
            vault_zero(&s_store, sizeof(s_store));
        }
        free(s);
    }
    return err;
}

/* Encrypt s_store with s_key and write it, with the parameters, as the
 * sealed blob. One write: a power loss keeps the old or the new blob. */
static esp_err_t write_sealed(void)
{
    sealed_t *s = malloc(sizeof(sealed_t));
    size_t n = store_size(s_store.count);
    esp_err_t err;

    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->params = s_params;
    esp_fill_random(s->nonce, NONCE_LEN);
    err = gcm(true, s_key, &s->params, s->nonce, (const uint8_t *)&s_store, s->data, n, s->tag);
    if (err == ESP_OK) {
        err = nvs_set_blob(s_nvs, KEY_SEALED, s, SEALED_HEAD + n);
    }
    free(s);
    return err;
}

static esp_err_t load(void)
{
    return s_has_pin ? load_sealed(s_key) : load_plain();
}

/* Write RAM to flash. If the write fails, restore RAM from flash. */
static esp_err_t save(void)
{
    esp_err_t err;

    s_store.version = VERSION;
    if (s_has_pin) {
        err = write_sealed();
    } else {
        err = nvs_set_blob(s_nvs, KEY_PLAIN, &s_store, store_size(s_store.count));
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
        load();
    }
    s_generation++;
    return err;
}

static void save_fails(void)
{
    if (nvs_set_u8(s_nvs, KEY_FAILS, s_fails) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the fail count");
    }
}

/* Seconds to wait after this many wrong PINs in a row. */
static uint32_t wait_s(int fails)
{
    uint32_t s = WAIT_MIN_S;

    if (fails < FAILS_FREE) {
        return 0;
    }
    for (int i = FAILS_FREE; i < fails && s < WAIT_MAX_S; i++) {
        s *= 2;
    }
    return s < WAIT_MAX_S ? s : WAIT_MAX_S;
}

static esp_err_t open_partition(void)
{
    esp_err_t err = nvs_flash_init_partition(PARTITION);

    /* A new partition can contain data from old firmware. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "partition is not empty NVS, erase it");
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(PARTITION), TAG, "erase");
        err = nvs_flash_init_partition(PARTITION);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init partition");
    return nvs_open_from_partition(PARTITION, NAMESPACE, NVS_READWRITE, &s_nvs);
}

/* With a sealed blob, the device has a PIN and starts locked. A plain
 * blob next to it is from a change of the PIN state that a power loss
 * stopped: both have the same entries. */
static esp_err_t load_state(void)
{
    sealed_t *s;
    size_t size;
    esp_err_t err = read_sealed(&s, &size);

    if (err == ESP_OK) {
        s_params = s->params;
        s_has_pin = true;
        s_locked = true;
        free(s);
        if (nvs_erase_key(s_nvs, KEY_PLAIN) == ESP_OK) {
            ESP_LOGW(TAG, "removed plain entries next to the sealed entries");
            nvs_commit(s_nvs);
        }
        nvs_get_u8(s_nvs, KEY_FAILS, &s_fails);
        if (s_fails >= FAILS_FREE) {
            s_allow_ms = now_ms() + wait_s(s_fails) * 1000;
        }
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;     /* Unknown sealed data: do not touch the flash. */
    }
    if (nvs_erase_key(s_nvs, KEY_SEALED) == ESP_OK) {
        nvs_commit(s_nvs);
    }
    return load_plain();
}

esp_err_t vault_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(open_partition(), TAG, "open");
    ESP_RETURN_ON_ERROR(load_state(), TAG, "load");
    if (s_has_pin) {
        ESP_LOGI(TAG, "locked, PIN key %s the eFuse key", s_params.bound ? "with" : "without");
    } else {
        ESP_LOGI(TAG, "%u entries, no PIN", s_store.count);
    }
    return ESP_OK;
}

/* ---- Entries ---- */

int vault_count(void)
{
    return s_store.count;
}

uint32_t vault_generation(void)
{
    return s_generation;
}

static void lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static esp_err_t unlock(esp_err_t err)
{
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

void vault_hold(void)
{
    lock();
}

void vault_release(void)
{
    unlock(ESP_OK);
}

static bool valid(int i)
{
    return i >= 0 && i < s_store.count;
}

esp_err_t vault_label(int i, char *buf, size_t size)
{
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    strlcpy(buf, s_store.entry[i].label, size);
    return unlock(ESP_OK);
}

esp_err_t vault_secret(int i, char *buf, size_t size)
{
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    strlcpy(buf, s_store.entry[i].secret, size);
    return unlock(ESP_OK);
}

esp_err_t vault_add(const char *label, const char *secret)
{
    return vault_add_many(&label, &secret, 1);
}

esp_err_t vault_add_many(const char *const labels[], const char *const secrets[], int n)
{
    for (int i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(vault_check_text(labels[i], VAULT_LABEL_MAX), TAG, "label");
        ESP_RETURN_ON_ERROR(vault_check_text(secrets[i], VAULT_SECRET_MAX), TAG, "secret");
    }
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (n > VAULT_MAX - s_store.count) {
        return unlock(ESP_ERR_NO_MEM);
    }
    for (int i = 0; i < n; i++) {
        entry_t *e = &s_store.entry[s_store.count++];

        strlcpy(e->label, labels[i], sizeof(e->label));
        strlcpy(e->secret, secrets[i], sizeof(e->secret));
        e->key = 0;
    }
    /* If the write fails, save() loads the old entries: nothing is added. */
    return unlock(save());
}

esp_err_t vault_set_label(int i, const char *label)
{
    ESP_RETURN_ON_ERROR(vault_check_text(label, VAULT_LABEL_MAX), TAG, "label");
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    vault_zero(s_store.entry[i].label, sizeof(s_store.entry[i].label));
    strlcpy(s_store.entry[i].label, label, sizeof(s_store.entry[i].label));
    return unlock(save());
}

esp_err_t vault_set_secret(int i, const char *secret)
{
    ESP_RETURN_ON_ERROR(vault_check_text(secret, VAULT_SECRET_MAX), TAG, "secret");
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    vault_zero(s_store.entry[i].secret, sizeof(s_store.entry[i].secret));
    strlcpy(s_store.entry[i].secret, secret, sizeof(s_store.entry[i].secret));
    return unlock(save());
}

uint8_t vault_key(int i)
{
    uint8_t key = 0;

    lock();
    if (valid(i)) {
        key = s_store.entry[i].key;
    }
    unlock(ESP_OK);
    return key;
}

int vault_find_key(uint8_t key)
{
    int found = -1;

    lock();
    for (int i = 0; key != 0 && i < s_store.count && found < 0; i++) {
        if (s_store.entry[i].key == key) {
            found = i;
        }
    }
    unlock(ESP_OK);
    return found;
}

esp_err_t vault_set_key(int i, uint8_t key)
{
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    for (int j = 0; key != 0 && j < s_store.count; j++) {
        if (s_store.entry[j].key == key) {
            s_store.entry[j].key = 0;
        }
    }
    s_store.entry[i].key = key;
    return unlock(save());
}

esp_err_t vault_delete(int i)
{
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    s_store.count--;
    memmove(&s_store.entry[i], &s_store.entry[i + 1], (s_store.count - i) * sizeof(entry_t));
    vault_zero(&s_store.entry[s_store.count], sizeof(entry_t));
    return unlock(save());
}

esp_err_t vault_move(int from, int to)
{
    entry_t tmp;

    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    if (!valid(from) || !valid(to)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    tmp = s_store.entry[from];
    if (from < to) {
        memmove(&s_store.entry[from], &s_store.entry[from + 1], (to - from) * sizeof(entry_t));
    } else {
        memmove(&s_store.entry[to + 1], &s_store.entry[to], (from - to) * sizeof(entry_t));
    }
    s_store.entry[to] = tmp;
    vault_zero(&tmp, sizeof(tmp));
    return unlock(save());
}

esp_err_t vault_wipe(void)
{
    esp_err_t err;

    lock();
    nvs_close(s_nvs);
    nvs_flash_deinit_partition(PARTITION);
    err = nvs_flash_erase_partition(PARTITION);
    if (err == ESP_OK) {
        err = open_partition();
    }
    vault_zero(&s_store, sizeof(s_store));
    vault_zero(s_key, sizeof(s_key));
    vault_zero(&s_params, sizeof(s_params));
    s_has_pin = false;
    s_locked = false;
    s_fails = 0;
    s_generation++;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wipe: %s", esp_err_to_name(err));
    }
    return unlock(err);
}

/* ---- PIN ---- */

bool vault_has_pin(void)
{
    return s_has_pin;
}

bool vault_locked(void)
{
    return s_locked;
}

int vault_fails(void)
{
    return s_fails;
}

bool vault_pin_bound(void)
{
    return s_has_pin && s_params.bound;
}

int vault_wait_s(void)
{
    int32_t left;

    if (!s_locked || s_fails < FAILS_FREE) {
        return 0;
    }
    left = (int32_t)(s_allow_ms - now_ms());
    return left > 0 ? (left + 999) / 1000 : 0;
}

esp_err_t vault_unlock(const char *pin)
{
    uint8_t key[AES_KEY_LEN];
    uint32_t start_ms;
    esp_err_t err;

    if (vault_check_pin(pin) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (!s_has_pin) {
        return unlock(ESP_ERR_INVALID_STATE);
    }
    if (!s_locked) {
        return unlock(ESP_OK);
    }
    if (vault_wait_s() > 0) {
        return unlock(ESP_ERR_TIMEOUT);
    }
    start_ms = now_ms();
    err = derive(pin, &s_params, key);
    if (err == ESP_OK) {
        err = load_sealed(key);
    }
    if (err == ESP_OK) {
        memcpy(s_key, key, sizeof(s_key));
        s_locked = false;
        if (s_fails != 0) {
            s_fails = 0;
            save_fails();
        }
        s_generation++;
        ESP_LOGI(TAG, "unlocked, %u entries, %lu ms", s_store.count, (unsigned long)(now_ms() - start_ms));
    } else if (err == ESP_ERR_INVALID_ARG) {
        if (s_fails < UINT8_MAX) {
            s_fails++;
        }
        save_fails();
        s_allow_ms = now_ms() + wait_s(s_fails) * 1000;
        ESP_LOGW(TAG, "wrong PIN, %u in a row", s_fails);
    }
    vault_zero(key, sizeof(key));
    return unlock(err);
}

void vault_lock(void)
{
    lock();
    if (s_has_pin && !s_locked) {
        vault_zero(&s_store, sizeof(s_store));
        vault_zero(s_key, sizeof(s_key));
        s_locked = true;
        s_generation++;
        ESP_LOGI(TAG, "locked");
    }
    unlock(ESP_OK);
}

esp_err_t vault_set_pin(const char *pin)
{
    params_t params = { .version = SEAL_VERSION, .iterations = ITERATIONS }, old_params;
    uint8_t old_key[AES_KEY_LEN];
    bool had_pin;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(vault_check_pin(pin), TAG, "pin");
    params.bound = efuse_key_id() >= 0;
    esp_fill_random(params.salt, SALT_LEN);
    lock();
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    had_pin = s_has_pin;
    old_params = s_params;
    memcpy(old_key, s_key, sizeof(old_key));
    err = derive(pin, &params, s_key);
    if (err == ESP_OK) {
        s_params = params;
        s_has_pin = true;
        s_store.version = VERSION;
        err = write_sealed();
    }
    if (err == ESP_OK) {
        nvs_erase_key(s_nvs, KEY_PLAIN);    /* Not there after a change of the PIN. */
        if (s_fails != 0) {
            s_fails = 0;
            nvs_erase_key(s_nvs, KEY_FAILS);
        }
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set pin: %s", esp_err_to_name(err));
        s_params = old_params;
        memcpy(s_key, old_key, sizeof(s_key));
        s_has_pin = had_pin;
    } else {
        ESP_LOGI(TAG, "PIN %s, key %s the eFuse key", had_pin ? "changed" : "set",
                 params.bound ? "with" : "without");
    }
    vault_zero(old_key, sizeof(old_key));
    s_generation++;
    return unlock(err);
}

esp_err_t vault_clear_pin(void)
{
    esp_err_t err;

    lock();
    if (!s_has_pin) {
        return unlock(ESP_OK);
    }
    if (s_locked) {
        return unlock(ESP_ERR_NOT_ALLOWED);
    }
    s_store.version = VERSION;
    err = nvs_set_blob(s_nvs, KEY_PLAIN, &s_store, store_size(s_store.count));
    if (err == ESP_OK) {
        nvs_erase_key(s_nvs, KEY_SEALED);
        nvs_erase_key(s_nvs, KEY_FAILS);
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clear pin: %s", esp_err_to_name(err));
    } else {
        s_has_pin = false;
        s_fails = 0;
        vault_zero(s_key, sizeof(s_key));
        vault_zero(&s_params, sizeof(s_params));
        ESP_LOGI(TAG, "PIN removed");
    }
    s_generation++;
    return unlock(err);
}
