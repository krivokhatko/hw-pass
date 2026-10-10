/* Bluetooth LE keypad: a HID keyboard (HID over GATT) paired with the
 * device. The NimBLE host runs the central role. The keypad task does
 * the steps of a connection one after the other. The host task gives it
 * the results, and gives the key changes to the UI.
 *
 * Protection:
 * - Only the buttons start a pairing (ui.c).
 * - The device connects only to the paired keypad, and only if it has
 *   the bonding keys. It uses the keys of the keypad only on a link that
 *   these keys encrypt. Outside a pairing, it refuses a passkey request
 *   and a new pairing, and the host does not pair again by itself
 *   (CMakeLists.txt): a connection cannot make new keys.
 * - In a pairing, the screen shows a random passkey, and the user types
 *   it on the keypad. This stops a man in the middle. A keypad without
 *   passkey entry pairs without this protection ("Just Works").
 * - Bluetooth starts only when a keypad is paired, or at a pairing. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "hidmap.h"
#include "keypad.h"

void keypad_key_name(uint8_t key, char buf[KEYPAD_KEY_NAME])
{
    hid_key_name(key, buf);
}

#if CONFIG_BT_NIMBLE_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "rng.h"

#if MYNEWT_VAL(BLE_RESTART_PAIR)
#error "BLE_RESTART_PAIR pairs again without the user: see CMakeLists.txt"
#endif

#define UUID_HID            0x1812
#define UUID_DEVICE_NAME    0x2A00
#define UUID_REPORT_MAP     0x2A4B
#define UUID_REPORT         0x2A4D
#define UUID_CCCD           0x2902
#define UUID_REPORT_REF     0x2908
#define REPORT_INPUT        1
#define APPEARANCE_MASK     0xFFC0
#define APPEARANCE_HID      0x03C0  /* Category 15: keyboard, mouse, and others */

#define FOREVER             UINT32_MAX
#define SYNC_MS             5000
#define SCAN_MS             60000   /* Pairing: time to find a keypad */
#define CONNECT_MS          10000
#define PAIR_MS             60000   /* Time to type the passkey */
#define ENCRYPT_MS          10000
#define GATT_MS             10000
#define STOP_MS             3000
#define RETRY_MS            5000    /* After an error on a link */
#define DATA_MAX            512     /* Bytes of a read value: the report map */
#define CHR_MAX             24      /* Characteristics of the HID service */
#define REPORT_MAX          64
#define EVENTS              16      /* Key changes that wait for the UI */
#define TASK_STACK          4096
#define PEER_VERSION        1

/* Event bits */
#define EV_SYNC         BIT0    /* The host is ready. */
#define EV_CONNECT      BIT1    /* A connection attempt ended. s_status: 0 if connected. */
#define EV_DISCONNECT   BIT2
#define EV_ENCRYPT      BIT3    /* The encryption changed. s_status */
#define EV_GATT         BIT4    /* A GATT procedure ended. s_status */
#define EV_FOUND        BIT5    /* The scan found a keypad: s_found */
#define EV_SCAN_END     BIT6
#define EV_CMD          BIT7    /* s_cmd has a request. */

typedef enum { CMD_NONE, CMD_PAIR, CMD_STOP, CMD_UNPAIR } cmd_t;

/* The paired keypad. NVS keeps it. With the handles, the keys of a
 * keypad work at once after a connection: the key that wakes the keypad
 * is not lost. */
typedef struct {
    uint8_t version;                    /* 0: no keypad */
    ble_addr_t addr;                    /* Identity address */
    char name[KEYPAD_NAME_MAX + 1];
    uint16_t report;                    /* Value handle of the keyboard input report */
    uint16_t cccd;                      /* Its client configuration descriptor */
    hid_keys_t keys;
} peer_t;

typedef struct {
    uint16_t def;       /* Handle of the declaration */
    uint16_t value;
    uint16_t uuid;      /* 0 if not a 16-bit UUID */
    uint8_t props;
} chr_t;

static const char *TAG = "keypad";

/* Fast scan for the keypad, 30 % of the time. A keypad sends
 * advertisements after a key wakes it. Short connection interval: fast
 * keys. The keypad can ask for a longer one. */
static const struct ble_gap_conn_params s_conn_params = {
    .scan_itvl = 160,               /* 100 ms */
    .scan_window = 48,              /* 30 ms */
    .itvl_min = 6,                  /* 7.5 ms */
    .itvl_max = 12,                 /* 15 ms */
    .supervision_timeout = 400,     /* 4 s */
};

static SemaphoreHandle_t s_lock;    /* s_peer, s_pair, s_cmd */
static EventGroupHandle_t s_events;
static QueueHandle_t s_queue;
static peer_t s_peer;
static keypad_pair_t s_pair;
static cmd_t s_cmd;
static volatile bool s_pairing;
static volatile keypad_state_t s_state;
static bool s_started;

/* The link. The host task changes it. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile int s_status;

/* Keys of the link. The keypad task sets them, then s_keys_ok. */
static volatile bool s_keys_ok;
static uint16_t s_report;
static hid_keys_t s_keys;
static hid_set_t s_down;            /* Host task only */

/* Results of the scan and the GATT procedures */
static ble_addr_t s_found;
static char s_found_name[KEYPAD_NAME_MAX + 1];
static uint16_t s_svc_start, s_svc_end;
static chr_t s_chr[CHR_MAX];
static int s_chr_count;
static uint16_t s_dsc_cccd, s_dsc_ref;
static uint8_t s_data[DATA_MAX];
static size_t s_data_len;

void ble_store_config_init(void);

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static uint32_t now_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}

/* Wait for one of the bits. Return all bits. */
static EventBits_t wait(EventBits_t bits, uint32_t ms)
{
    return xEventGroupWaitBits(s_events, bits, pdFALSE, pdFALSE,
                               ms == FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms));
}

static void request(cmd_t cmd)
{
    lock();
    s_cmd = cmd;
    xEventGroupSetBits(s_events, EV_CMD);
    unlock();
}

/* A pairing starts here: from now on, a stop stops it. */
static cmd_t take_cmd(void)
{
    cmd_t cmd;

    lock();
    cmd = s_cmd;
    s_cmd = CMD_NONE;
    s_pairing = cmd == CMD_PAIR;
    xEventGroupClearBits(s_events, EV_CMD);
    unlock();
    return cmd;
}

static void set_pair(keypad_pair_step_t step, const char *name, const char *error)
{
    lock();
    s_pair.step = step;
    s_pair.passkey = 0;
    if (name) {
        strlcpy(s_pair.name, name, sizeof(s_pair.name));
    }
    s_pair.error = error;
    unlock();
}

/* A command stops the current step. */
static bool stopped(void)
{
    return xEventGroupGetBits(s_events) & EV_CMD;
}

/* Printable ASCII only. */
static void copy_name(char *dst, const uint8_t *src, size_t len)
{
    size_t n = len < KEYPAD_NAME_MAX ? len : KEYPAD_NAME_MAX;

    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i] >= 0x20 && src[i] <= 0x7E ? src[i] : '?';
    }
    dst[n] = '\0';
}

static void addr_name(char *dst, const ble_addr_t *a)
{
    snprintf(dst, KEYPAD_NAME_MAX + 1, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

/* ---- Paired keypad in NVS ---- */

static void load_peer(void)
{
    nvs_handle_t h;
    size_t size = sizeof(s_peer);

    if (nvs_open("keypad", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "peer", &s_peer, &size) != ESP_OK || size != sizeof(s_peer) ||
            s_peer.version != PEER_VERSION) {
            memset(&s_peer, 0, sizeof(s_peer));
        }
        nvs_close(h);
    }
}

static esp_err_t save_peer(const peer_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("keypad", NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = p->version ? nvs_set_blob(h, "peer", p, sizeof(*p)) : nvs_erase_key(h, "peer");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: %s", esp_err_to_name(err));
    }
    lock();
    s_peer = *p;
    unlock();
    return err;
}

static void get_peer(peer_t *p)
{
    lock();
    *p = s_peer;
    unlock();
}

static bool have_peer(void)
{
    return s_peer.version != 0;
}

/* Remove the keypad and all bonding keys. */
static void forget(void)
{
    peer_t none = { 0 };

    if (s_started) {
        ble_store_clear();
    }
    save_peer(&none);
    s_state = KEYPAD_NONE;
    ESP_LOGI(TAG, "no keypad");
}

/* ---- Host task: GAP events ---- */

static void post_key(uint8_t key, bool down)
{
    keypad_event_t ev = { .key = key, .down = down, .ms = now_ms() };

    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        /* A lost "up" must not become a hold: cancel all keys. */
        ESP_LOGW(TAG, "key queue full");
        ev = (keypad_event_t) { .ms = ev.ms };
        xQueueReset(s_queue);
        xQueueSend(s_queue, &ev, 0);
    }
}

/* Send the changes since the last report. */
static void set_keys(const hid_set_t *down)
{
    for (int k = 0; k < 256; k++) {
        if (hid_set_has(down, k) != hid_set_has(&s_down, k)) {
            post_key(k, hid_set_has(down, k));
        }
    }
    s_down = *down;
}

/* The link is encrypted with the bonding keys. */
static bool trusted(uint16_t conn)
{
    struct ble_gap_conn_desc desc;

    return ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.bonded;
}

static void on_report(struct os_mbuf *om)
{
    uint8_t buf[REPORT_MAX];
    uint16_t len = OS_MBUF_PKTLEN(om) < sizeof(buf) ? OS_MBUF_PKTLEN(om) : sizeof(buf);
    hid_set_t down;

    if (os_mbuf_copydata(om, 0, len, buf) != 0) {
        return;
    }
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, len, ESP_LOG_DEBUG);
    if (hid_keys_down(&s_keys, buf, len, &down)) {
        set_keys(&down);
    }
}

/* A HID device in pairing mode: discoverable and connectable. */
static void on_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    char name[KEYPAD_NAME_MAX + 1] = "";
    bool hid = false;

    if (d->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
        ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 ||
        !(f.flags & (BLE_HS_ADV_F_DISC_LTD | BLE_HS_ADV_F_DISC_GEN))) {
        return;
    }
    for (int i = 0; i < f.num_uuids16; i++) {
        hid |= ble_uuid_u16(&f.uuids16[i].u) == UUID_HID;
    }
    hid |= f.appearance_is_present && (f.appearance & APPEARANCE_MASK) == APPEARANCE_HID;
    if (f.name) {
        copy_name(name, f.name, f.name_len);
    }
    ESP_LOGI(TAG, "found %02X:%02X:%02X:%02X:%02X:%02X (type %u) \"%s\" %d dBm%s",
             d->addr.val[5], d->addr.val[4], d->addr.val[3], d->addr.val[2], d->addr.val[1],
             d->addr.val[0], d->addr.type, name, d->rssi, hid ? ", HID" : "");
    if (hid && !(xEventGroupGetBits(s_events) & EV_FOUND)) {
        s_found = d->addr;
        if (name[0]) {
            strcpy(s_found_name, name);
        } else {
            addr_name(s_found_name, &d->addr);
        }
        xEventGroupSetBits(s_events, EV_FOUND);
    }
}

/* Passkey entry: the screen shows the passkey, the user types it on the
 * keypad. Other methods need a display or input on the keypad side. */
static void on_passkey(uint16_t conn, uint8_t action)
{
    struct ble_sm_io io = { .action = action };
    uint32_t r;

    if (!s_pairing || action != BLE_SM_IOACT_DISP) {
        ESP_LOGW(TAG, "refuse passkey action %u", action);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    rng_bytes(&r, sizeof(r));
    io.passkey = r % 1000000;
    lock();
    s_pair.passkey = io.passkey;
    s_pair.step = KEYPAD_PAIR_PASSKEY;
    unlock();
    ble_sm_inject_io(conn, &io);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        on_adv(&event->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xEventGroupSetBits(s_events, EV_SCAN_END);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_status = event->connect.status;
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
        }
        xEventGroupSetBits(s_events, EV_CONNECT);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: reason 0x%X", event->disconnect.reason);
        s_keys_ok = false;
        /* All keys up, but not as a push: a lost link must not type. */
        memset(&s_down, 0, sizeof(s_down));
        post_key(0, false);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(s_events, EV_DISCONNECT);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_status = event->enc_change.status;
        xEventGroupSetBits(s_events, EV_ENCRYPT);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        on_passkey(event->passkey.conn_handle, event->passkey.params.action);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The keypad lost its keys and pairs again: only in a pairing. */
        if (!s_pairing) {
            ESP_LOGW(TAG, "refuse a new pairing");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (s_keys_ok && event->notify_rx.attr_handle == s_report && trusted(event->notify_rx.conn_handle)) {
            on_report(event->notify_rx.om);
        }
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    xEventGroupSetBits(s_events, EV_SYNC);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset: %d", reason);
    xEventGroupClearBits(s_events, EV_SYNC);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- Host task: GATT results ---- */

static int gatt_done(int status)
{
    s_status = status == BLE_HS_EDONE ? 0 : status;
    xEventGroupSetBits(s_events, EV_GATT);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status != 0) {
        return gatt_done(error->status);
    }
    if (s_svc_start == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status != 0) {
        return gatt_done(error->status);
    }
    if (s_chr_count < CHR_MAX) {
        s_chr[s_chr_count++] = (chr_t) {
            .def = chr->def_handle,
            .value = chr->val_handle,
            .uuid = ble_uuid_u16(&chr->uuid.u),
            .props = chr->properties,
        };
    }
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_value,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status != 0) {
        return gatt_done(error->status);
    }
    if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) {
        s_dsc_cccd = dsc->handle;
    } else if (ble_uuid_u16(&dsc->uuid.u) == UUID_REPORT_REF) {
        s_dsc_ref = dsc->handle;
    }
    return 0;
}

/* Long reads give the value in parts. A read by UUID gives the first
 * attribute that has the UUID. */
static int on_read(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        return gatt_done(error->status);
    }
    if (attr && (arg == NULL || s_data_len == 0)) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);

        if (n > sizeof(s_data) - s_data_len) {
            n = sizeof(s_data) - s_data_len;
        }
        os_mbuf_copydata(attr->om, 0, n, s_data + s_data_len);
        s_data_len += n;
    }
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    return gatt_done(error->status);
}

/* ---- Keypad task: the steps of a link ---- */

/* Wait for the end of the GATT procedure that rc started. After a
 * timeout, end the link: a late result must not count for the next one. */
static int gatt_wait(int rc)
{
    if (rc != 0) {
        return rc;
    }
    if (!(wait(EV_GATT, GATT_MS) & EV_GATT)) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return BLE_HS_ETIMEOUT;
    }
    xEventGroupClearBits(s_events, EV_GATT);
    return s_status;
}

static int read_value(uint16_t handle)
{
    s_data_len = 0;
    return gatt_wait(ble_gattc_read_long(s_conn, handle, 0, on_read, NULL));
}

static int read_uuid(uint16_t uuid)
{
    s_data_len = 0;
    return gatt_wait(ble_gattc_read_by_uuid(s_conn, 1, 0xFFFF, BLE_UUID16_DECLARE(uuid), on_read, (void *)1));
}

static int find_chr(uint16_t uuid)
{
    for (int i = 0; i < s_chr_count; i++) {
        if (s_chr[i].uuid == uuid) {
            return i;
        }
    }
    return -1;
}

/* Find the keyboard input report: its value handle, its client
 * configuration descriptor, and the position of the keys. */
static esp_err_t discover(peer_t *p)
{
    hid_keys_t keys;
    int map, rc;

    s_svc_start = s_svc_end = 0;
    rc = gatt_wait(ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(UUID_HID), on_svc, NULL));
    if (rc != 0 || s_svc_start == 0) {
        ESP_LOGE(TAG, "no HID service: %d", rc);
        return ESP_ERR_NOT_FOUND;
    }
    s_chr_count = 0;
    rc = gatt_wait(ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_chr, NULL));
    map = find_chr(UUID_REPORT_MAP);
    if (rc != 0 || map < 0 || (rc = read_value(s_chr[map].value)) != 0) {
        ESP_LOGE(TAG, "no report map: %d, %d characteristics", rc, s_chr_count);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "report map, %u bytes:", (unsigned)s_data_len);
    ESP_LOG_BUFFER_HEX(TAG, s_data, s_data_len);
    if (!hid_parse_map(s_data, s_data_len, &keys)) {
        ESP_LOGE(TAG, "the report map has no keyboard");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "keys: report ID %u, %s of %u at bit %u", keys.id, keys.bitmap ? "bitmap" : "array",
             keys.count, keys.offset);

    for (int i = 0; i < s_chr_count && p->report == 0; i++) {
        uint16_t end = i + 1 < s_chr_count ? s_chr[i + 1].def - 1 : s_svc_end;
        uint8_t id = 0, type = REPORT_INPUT;   /* Without a reference: the only report */

        if (s_chr[i].uuid != UUID_REPORT || !(s_chr[i].props & BLE_GATT_CHR_PROP_NOTIFY)) {
            continue;
        }
        s_dsc_cccd = s_dsc_ref = 0;
        if (gatt_wait(ble_gattc_disc_all_dscs(s_conn, s_chr[i].value, end, on_dsc, NULL)) != 0 ||
            s_dsc_cccd == 0) {
            continue;
        }
        if (s_dsc_ref != 0) {
            if (read_value(s_dsc_ref) != 0 || s_data_len < 2) {
                continue;
            }
            id = s_data[0];
            type = s_data[1];
        }
        ESP_LOGI(TAG, "report 0x%04X: ID %u, type %u", s_chr[i].value, id, type);
        if (type == REPORT_INPUT && id == keys.id) {
            p->report = s_chr[i].value;
            p->cccd = s_dsc_cccd;
            p->keys = keys;
        }
    }
    if (p->report == 0) {
        ESP_LOGE(TAG, "no input report with ID %u", keys.id);
        return ESP_ERR_NOT_FOUND;
    }
    if (read_uuid(UUID_DEVICE_NAME) == 0 && s_data_len > 0) {
        copy_name(p->name, s_data, s_data_len);
    }
    return ESP_OK;
}

/* Connect to addr. Return 0 when connected, BLE_HS_EAPP after a command. */
static int connect(const ble_addr_t *addr, int32_t ms)
{
    int rc;

    xEventGroupClearBits(s_events, EV_CONNECT | EV_DISCONNECT | EV_GATT);
    rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, ms, &s_conn_params, gap_event, NULL);
    if (rc != 0) {
        return rc;
    }
    if (wait(EV_CONNECT | EV_CMD, FOREVER) & EV_CONNECT) {
        return s_status;
    }
    /* A command. The cancel ends the attempt, or the link came first. */
    ble_gap_conn_cancel();
    wait(EV_CONNECT, STOP_MS);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        wait(EV_DISCONNECT, STOP_MS);
    }
    return BLE_HS_EAPP;
}

static void disconnect(void)
{
    uint16_t conn = s_conn;

    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait(EV_DISCONNECT, STOP_MS);
    }
}

/* Encrypt the link: with the bonding keys, or in a pairing with new keys.
 * The keypad can ask for this itself after the connection. */
static int encrypt(uint32_t ms)
{
    struct ble_gap_conn_desc desc;
    int rc;

    xEventGroupClearBits(s_events, EV_ENCRYPT);
    if (ble_gap_conn_find(s_conn, &desc) == 0 && desc.sec_state.encrypted) {
        return 0;
    }
    rc = ble_gap_security_initiate(s_conn);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return rc;
    }
    if (!(wait(EV_ENCRYPT | EV_DISCONNECT | EV_CMD, ms) & EV_ENCRYPT)) {
        return BLE_HS_ETIMEOUT;
    }
    return s_status;
}

/* The keypad has bonding keys with an LTK. Without the LTK, the host
 * pairs again at the encryption: a connection would make new keys. */
static bool bonded(const ble_addr_t *addr)
{
    struct ble_store_key_sec key = { .peer_addr = *addr };
    struct ble_store_value_sec value;

    return ble_store_read_peer_sec(&key, &value) == 0 && value.ltk_present;
}

/* Use the keys of the link. The keypad keeps the subscription in its
 * bonding data: it can send a key before the write. */
static int use_keys(const peer_t *p)
{
    static const uint8_t notify[2] = { 1, 0 };
    int rc;

    s_report = p->report;
    s_keys = p->keys;
    __sync_synchronize();   /* The host task sees the keys before s_keys_ok. */
    s_keys_ok = true;
    rc = gatt_wait(ble_gattc_write_flat(s_conn, p->cccd, notify, sizeof(notify), on_write, NULL));
    if (rc != 0) {
        s_keys_ok = false;
        return rc;
    }
    s_state = KEYPAD_CONNECTED;
    ESP_LOGI(TAG, "connected: %s", p->name);
    return 0;
}

/* Wait until the link ends or a command comes. */
static void hold_link(void)
{
    for (;;) {
        if (wait(EV_DISCONNECT | EV_CMD, FOREVER) & EV_DISCONNECT) {
            break;
        }
        lock();
        if (s_cmd == CMD_STOP) {    /* Late stop of a pairing that is done */
            s_cmd = CMD_NONE;
            xEventGroupClearBits(s_events, EV_CMD);
            unlock();
            continue;
        }
        unlock();
        disconnect();
        break;
    }
    s_state = have_peer() ? KEYPAD_WAITING : KEYPAD_NONE;
}

/* Connect to the paired keypad and use its keys until the link ends. */
static void serve(void)
{
    peer_t p;
    int rc;

    get_peer(&p);
    if (!bonded(&p.addr)) {
        ESP_LOGW(TAG, "no bonding keys: pair the keypad again");
        forget();
        return;
    }
    s_state = KEYPAD_WAITING;
    rc = connect(&p.addr, BLE_HS_FOREVER);
    if (rc == BLE_HS_EAPP) {
        return;
    }
    if (rc == 0) {
        rc = encrypt(ENCRYPT_MS);
    }
    if (rc == 0 && !trusted(s_conn)) {
        rc = BLE_HS_EAUTHEN;
    }
    if (rc == 0) {
        rc = use_keys(&p);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "link: %d", rc);
        disconnect();
        s_state = KEYPAD_WAITING;
        wait(EV_CMD, RETRY_MS);
        return;
    }
    hold_link();
}

/* Remove all bonding keys, except the keys of addr. */
static void keep_only(const ble_addr_t *addr)
{
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;

    if (ble_store_util_bonded_peers(peers, &n, sizeof(peers) / sizeof(peers[0])) == 0) {
        for (int i = 0; i < n; i++) {
            if (ble_addr_cmp(&peers[i], addr) != 0) {
                ble_store_util_delete_peer(&peers[i]);
            }
        }
    }
}

static int scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 160,                /* 100 ms */
        .window = 80,               /* 50 ms */
        .filter_duplicates = 1,
    };
    EventBits_t bits;
    int rc;

    xEventGroupClearBits(s_events, EV_FOUND | EV_SCAN_END);
    rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, SCAN_MS, &params, gap_event, NULL);
    if (rc != 0) {
        return rc;
    }
    bits = wait(EV_FOUND | EV_SCAN_END | EV_CMD, FOREVER);
    ble_gap_disc_cancel();
    return (bits & EV_CMD) ? BLE_HS_EAPP : (bits & EV_FOUND) ? 0 : BLE_HS_ETIMEOUT;
}

/* Find a keypad in pairing mode, pair it, and replace the old keypad. */
static void pair(void)
{
    struct ble_gap_conn_desc desc;
    peer_t old, p = { .version = PEER_VERSION };
    const char *error;
    bool found = false;
    int rc;

    get_peer(&old);
    set_pair(KEYPAD_PAIR_SEARCHING, "", NULL);
    rc = scan();
    if (rc != 0) {
        error = rc == BLE_HS_ETIMEOUT ? "No keypad found" : "Bluetooth error";
        goto end;
    }
    found = true;
    set_pair(KEYPAD_PAIR_CONNECTING, s_found_name, NULL);
    ESP_LOGI(TAG, "pair %s", s_found_name);
    strcpy(p.name, s_found_name);
    p.addr = s_found;
    ble_store_util_delete_peer(&p.addr);    /* Old keys of this keypad */
    rc = connect(&p.addr, CONNECT_MS);
    if (rc != 0) {
        error = "Cannot connect";
        goto end;
    }
    rc = encrypt(PAIR_MS);
    if (rc != 0 || ble_gap_conn_find(s_conn, &desc) != 0 || !desc.sec_state.bonded ||
        !bonded(&desc.peer_id_addr)) {
        ESP_LOGE(TAG, "pairing: %d", rc);
        error = "Pairing failed";
        goto end;
    }
    ESP_LOGI(TAG, "paired: authenticated %u, key size %u", desc.sec_state.authenticated,
             desc.sec_state.key_size);
    p.addr = desc.peer_id_addr;
    if (discover(&p) != ESP_OK) {
        error = "Not a keyboard";
        goto end;
    }
    rc = use_keys(&p);
    if (rc != 0) {
        error = "Pairing failed";
        goto end;
    }
    /* A stop before this point stops the pairing. A later stop does nothing. */
    lock();
    if (stopped()) {
        unlock();
        error = "Stopped";
        goto end;
    }
    s_pairing = false;
    unlock();
    keep_only(&p.addr);
    save_peer(&p);
    set_pair(KEYPAD_PAIR_DONE, p.name, NULL);
    hold_link();
    return;

end:
    if (stopped()) {
        error = "Stopped";
    }
    s_pairing = false;
    disconnect();
    if (found) {
        ble_store_util_delete_peer(&p.addr);
        if (have_peer() && ble_addr_cmp(&p.addr, &old.addr) == 0) {
            forget();   /* The same keypad: its old keys are gone. */
        }
    }
    ESP_LOGW(TAG, "pairing: %s", error);
    set_pair(KEYPAD_PAIR_FAILED, NULL, error);
}

static esp_err_t start(void)
{
    esp_err_t err;

    if (!s_started) {
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
        ble_store_config_init();
        nimble_port_freertos_init(host_task);
        s_started = true;
        ESP_LOGI(TAG, "Bluetooth on");
    }
    return (wait(EV_SYNC, SYNC_MS) & EV_SYNC) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void keypad_task(void *arg)
{
    for (;;) {
        cmd_t cmd = take_cmd();

        if (cmd == CMD_UNPAIR) {    /* Also when Bluetooth does not start */
            forget();
            continue;
        }
        if ((cmd == CMD_PAIR || have_peer()) && start() != ESP_OK) {
            if (cmd == CMD_PAIR) {
                s_pairing = false;
                set_pair(KEYPAD_PAIR_FAILED, NULL, "Bluetooth error");
            }
            /* The host can become ready later, for example after a reset. */
            wait(EV_CMD | EV_SYNC, FOREVER);
            continue;
        }
        if (cmd == CMD_PAIR) {
            pair();
        } else if (have_peer()) {
            serve();
        } else {
            wait(EV_CMD, FOREVER);
        }
    }
}

/* ---- API ---- */

esp_err_t keypad_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_queue = xQueueCreate(EVENTS, sizeof(keypad_event_t));
    if (!s_lock || !s_events || !s_queue) {
        return ESP_ERR_NO_MEM;
    }
    load_peer();
    s_state = have_peer() ? KEYPAD_WAITING : KEYPAD_NONE;
    if (have_peer()) {
        ESP_LOGI(TAG, "paired: %s", s_peer.name);
    }
    return xTaskCreate(keypad_task, "keypad", TASK_STACK, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

keypad_state_t keypad_state(void)
{
    return s_state;
}

void keypad_name(char *buf, size_t size)
{
    lock();
    strlcpy(buf, s_peer.name, size);
    unlock();
}

esp_err_t keypad_pair_start(void)
{
    if (!s_events) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.step = KEYPAD_PAIR_SEARCHING;
    unlock();
    request(CMD_PAIR);
    return ESP_OK;
}

void keypad_pair_status(keypad_pair_t *pair)
{
    lock();
    *pair = s_pair;
    unlock();
}

void keypad_pair_stop(void)
{
    lock();
    if (s_pairing || s_cmd == CMD_PAIR) {
        s_cmd = CMD_STOP;
        xEventGroupSetBits(s_events, EV_CMD);
    }
    unlock();
}

esp_err_t keypad_unpair(void)
{
    if (!s_events || !have_peer()) {
        return ESP_ERR_NOT_FOUND;
    }
    request(CMD_UNPAIR);
    for (int ms = 0; have_peer(); ms += 100) {
        if (ms >= STOP_MS * 2) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

bool keypad_event(keypad_event_t *ev)
{
    return s_queue && xQueueReceive(s_queue, ev, 0) == pdTRUE;
}

void keypad_flush(void)
{
    if (s_queue) {
        xQueueReset(s_queue);
    }
}

#else   /* CONFIG_BT_NIMBLE_ENABLED */

esp_err_t keypad_init(void)
{
    return ESP_OK;
}

keypad_state_t keypad_state(void)
{
    return KEYPAD_NONE;
}

void keypad_name(char *buf, size_t size)
{
    if (size > 0) {
        buf[0] = '\0';
    }
}

esp_err_t keypad_pair_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void keypad_pair_status(keypad_pair_t *pair)
{
    *pair = (keypad_pair_t) { .step = KEYPAD_PAIR_FAILED, .error = "Not in this build" };
}

void keypad_pair_stop(void)
{
}

esp_err_t keypad_unpair(void)
{
    return ESP_ERR_NOT_FOUND;
}

bool keypad_event(keypad_event_t *ev)
{
    return false;
}

void keypad_flush(void)
{
}

#endif  /* CONFIG_BT_NIMBLE_ENABLED */
