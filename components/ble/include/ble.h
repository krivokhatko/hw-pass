#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gatt.h"
#include "nimble/ble.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The Bluetooth LE host (NimBLE), shared by the keypad link (central
 * role, keypad) and the computer link (peripheral role, blehid). This
 * module starts the host when the first link needs it, sets the pairing
 * method, and keeps the bonding keys in NVS. The init functions run in
 * the main task, before the host starts. */

#define BLE_NAME        "hw-pass"   /* Device name in the GAP service and the advertisements */
#define BLE_ADDR_TEXT   18          /* Buffer size for ble_addr_text */

/* The host became ready (true), or it reset (false). Runs in the host task. */
typedef void (*ble_sync_cb_t)(bool ready);

esp_err_t ble_init(void);

#if CONFIG_BT_NIMBLE_ENABLED

/* Get the sync events. Call before ble_start. At most 2 users. */
esp_err_t ble_on_sync(ble_sync_cb_t cb);

/* The GATT services of the device: one table, with a zero entry at the
 * end. Call before the first ble_start: the host cannot add services after
 * its start. */
esp_err_t ble_add_services(const struct ble_gatt_svc_def *svcs);

/* Start the host if it does not run, and wait until it is ready. Safe to
 * call from different tasks, and again after the start. */
esp_err_t ble_start(void);

bool ble_started(void);

/* The host runs and is ready for GAP and GATT calls. */
bool ble_ready(void);

/* The peer has bonding keys with an LTK: a link with it can be encrypted
 * without a new pairing. */
bool ble_bonded(const ble_addr_t *addr);

/* "AA:BB:CC:DD:EE:FF" */
void ble_addr_text(char *buf, size_t size, const ble_addr_t *addr);

#endif  /* CONFIG_BT_NIMBLE_ENABLED */

#ifdef __cplusplus
}
#endif
