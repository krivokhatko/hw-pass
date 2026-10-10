/* Password entries. RAM holds a copy. Each change writes all entries
 * as one NVS blob, thus a power loss keeps the old or the new set. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "vault.h"

#define PARTITION   "vault"
#define NAMESPACE   "vault"
#define KEY         "entries"
#define VERSION     1

typedef struct {
    char label[VAULT_LABEL_MAX + 1];
    char secret[VAULT_SECRET_MAX + 1];
} entry_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    uint8_t reserved[2];
    entry_t entry[VAULT_MAX];
} store_t;

static const char *TAG = "vault";
static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static store_t s_store;
static uint32_t s_generation;

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

static size_t store_size(int count)
{
    return offsetof(store_t, entry) + count * sizeof(entry_t);
}

static esp_err_t load(void)
{
    size_t size = sizeof(s_store);
    esp_err_t err;

    vault_zero(&s_store, sizeof(s_store));
    err = nvs_get_blob(s_nvs, KEY, &s_store, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read");
    if (s_store.version != VERSION || s_store.count > VAULT_MAX || size != store_size(s_store.count)) {
        ESP_LOGE(TAG, "unknown data: version %u, %u entries, %u bytes",
                 s_store.version, s_store.count, (unsigned)size);
        vault_zero(&s_store, sizeof(s_store));
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

/* Write RAM to flash. If the write fails, restore RAM from flash. */
static esp_err_t save(void)
{
    esp_err_t err;

    s_store.version = VERSION;
    err = nvs_set_blob(s_nvs, KEY, &s_store, store_size(s_store.count));
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

esp_err_t vault_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(open_partition(), TAG, "open");
    ESP_RETURN_ON_ERROR(load(), TAG, "load");
    ESP_LOGI(TAG, "%u entries", s_store.count);
    return ESP_OK;
}

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
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    strlcpy(buf, s_store.entry[i].label, size);
    return unlock(ESP_OK);
}

esp_err_t vault_secret(int i, char *buf, size_t size)
{
    lock();
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
    if (n > VAULT_MAX - s_store.count) {
        return unlock(ESP_ERR_NO_MEM);
    }
    for (int i = 0; i < n; i++) {
        entry_t *e = &s_store.entry[s_store.count++];

        strlcpy(e->label, labels[i], sizeof(e->label));
        strlcpy(e->secret, secrets[i], sizeof(e->secret));
    }
    /* If the write fails, save() loads the old entries: nothing is added. */
    return unlock(save());
}

esp_err_t vault_set_label(int i, const char *label)
{
    ESP_RETURN_ON_ERROR(vault_check_text(label, VAULT_LABEL_MAX), TAG, "label");
    lock();
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
    if (!valid(i)) {
        return unlock(ESP_ERR_NOT_FOUND);
    }
    vault_zero(s_store.entry[i].secret, sizeof(s_store.entry[i].secret));
    strlcpy(s_store.entry[i].secret, secret, sizeof(s_store.entry[i].secret));
    return unlock(save());
}

esp_err_t vault_delete(int i)
{
    lock();
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
    s_generation++;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wipe: %s", esp_err_to_name(err));
    }
    return unlock(err);
}
