/* Bluetooth LE host for the two links of the device: the keypad (central
 * role, keypad.c) and the computer (peripheral role, blehid.c). One
 * NimBLE host serves both. The links can be up at the same time, and the
 * device can scan for a keypad while it advertises for a computer.
 *
 * Pairing: the device has a screen and no keys. It shows a passkey, and
 * the other side types it. This stops a man in the middle. The host asks
 * for this protection in each pairing. Each link checks the result of its
 * pairing itself, and refuses a pairing that the user did not start. The
 * bonding keys stay in NVS (ble_store_config). Bluetooth starts only when
 * a link needs it: a paired keypad or computer, or a pairing. */
#include <stdio.h>
#include "sdkconfig.h"
#include "ble.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "rng.h"

#if MYNEWT_VAL(BLE_RESTART_PAIR)
#error "BLE_RESTART_PAIR pairs again without the user: see CMakeLists.txt"
#endif

#define SYNC_MS     5000
#define USERS       2
#define EV_SYNC     BIT0

static const char *TAG = "ble";
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static const struct ble_gatt_svc_def *s_svcs;
static ble_sync_cb_t s_users[USERS];
static int s_user_count;
static bool s_started;

void ble_store_config_init(void);

static void tell_users(bool ready)
{
    for (int i = 0; i < s_user_count; i++) {
        s_users[i](ready);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    xEventGroupSetBits(s_events, EV_SYNC);
    tell_users(true);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset: %d", reason);
    xEventGroupClearBits(s_events, EV_SYNC);
    tell_users(false);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    return s_lock && s_events ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ble_on_sync(ble_sync_cb_t cb)
{
    if (s_user_count >= USERS) {
        return ESP_ERR_NO_MEM;
    }
    s_users[s_user_count++] = cb;
    return ESP_OK;
}

esp_err_t ble_add_services(const struct ble_gatt_svc_def *svcs)
{
    if (s_started || s_svcs) {
        return ESP_ERR_INVALID_STATE;
    }
    s_svcs = svcs;
    return ESP_OK;
}

/* The host, its services, and its task. */
static esp_err_t start(void)
{
    esp_err_t err;
    int rc;

    rng_radio_lock();
    err = nimble_port_init();
    rng_radio_unlock(RNG_RADIO_BT, err == ESP_OK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth start: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_NAME);
    if (s_svcs) {
        rc = ble_gatts_count_cfg(s_svcs);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(s_svcs);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "services: %d", rc);
        }
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "Bluetooth on");
    return ESP_OK;
}

esp_err_t ble_start(void)
{
    esp_err_t err = ESP_OK;

    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_started) {
        err = start();
        s_started = err == ESP_OK;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    return (xEventGroupWaitBits(s_events, EV_SYNC, pdFALSE, pdFALSE, pdMS_TO_TICKS(SYNC_MS)) & EV_SYNC) ?
           ESP_OK : ESP_ERR_TIMEOUT;
}

bool ble_started(void)
{
    return s_started;
}

bool ble_ready(void)
{
    return s_events && (xEventGroupGetBits(s_events) & EV_SYNC);
}

bool ble_bonded(const ble_addr_t *addr)
{
    struct ble_store_key_sec key = { .peer_addr = *addr };
    struct ble_store_value_sec value;

    return ble_store_read_peer_sec(&key, &value) == 0 && value.ltk_present;
}

void ble_addr_text(char *buf, size_t size, const ble_addr_t *a)
{
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

#else   /* CONFIG_BT_NIMBLE_ENABLED */

esp_err_t ble_init(void)
{
    return ESP_OK;
}

#endif  /* CONFIG_BT_NIMBLE_ENABLED */
