/* Bluetooth LE keyboard and mouse for a computer: HID over GATT in the
 * peripheral role of the shared host (ble.c). A laptop without a USB
 * cable gets the typed passwords, Enter and Backspace of the keypad, and
 * the moves of the mouse mover (hid.c selects the link). The device keeps
 * one paired computer in NVS and advertises while no computer is
 * connected: the paired computer connects by itself when it sees the
 * device. The host task runs the events of the link; this module has no
 * task of its own. The Battery service gives the level of the battery of
 * the device (battery.c): the computer shows it, and gets each change.
 *
 * Protection:
 * - Only a request of the user starts a pairing (ui.c): the buttons, or
 *   the command line. Then the device advertises as discoverable for
 *   PAIR_MS. At other times, the advertisements are not discoverable: the
 *   paired computer knows the address.
 * - The pairing needs a passkey: the screen shows it, the user types it on
 *   the computer. A pairing without a passkey gives keys that are not
 *   authenticated: the link ends, and the computer is not paired.
 * - Outside a pairing, the device refuses a passkey request and a new
 *   pairing. It sends keys only on a link that the bonding keys of the
 *   paired computer encrypt. A link without this encryption ends after
 *   ENCRYPT_MS.
 * - The report map and the reports need an encrypted link. A computer
 *   without the keys cannot read them. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "blehid.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "battery.h"
#include "ble.h"
#include "rng.h"
#include "usbdev.h"

#define UUID_HID            0x1812
#define UUID_BATTERY        0x180F
#define UUID_DEVICE_INFO    0x180A
#define UUID_DEVICE_NAME    0x2A00
#define UUID_BATTERY_LEVEL  0x2A19
#define UUID_HID_INFO       0x2A4A
#define UUID_REPORT_MAP     0x2A4B
#define UUID_CONTROL_POINT  0x2A4C
#define UUID_REPORT         0x2A4D
#define UUID_PROTOCOL_MODE  0x2A4E
#define UUID_PNP_ID         0x2A50
#define UUID_REPORT_REF     0x2908

#define APPEARANCE_KEYBOARD 0x03C1
#define REPORT_KEYBOARD     1       /* Report IDs in the report map */
#define REPORT_MOUSE        2
#define REPORT_INPUT        1       /* Report types in the reference descriptors */
#define REPORT_OUTPUT       2
#define KEYS_LEN            8       /* Modifiers, reserved, 6 keys */
#define MOUSE_LEN           3       /* Buttons, X, Y */
#define LED_CAPS_LOCK       0x02
#define PROTOCOL_REPORT     1
#define BATTERY_UNKNOWN     100     /* Level in the report when there is no measurement */

#define ADV_ITVL_MIN        160     /* 100 ms */
#define ADV_ITVL_MAX        240     /* 150 ms */
#define PAIR_MS             60000   /* Time for the computer to connect and pair */
#define ENCRYPT_MS          10000   /* A link without encryption ends after this time. */
#define SEND_WAIT_MS        200     /* Time for a free buffer of a key report */
#define HOST_VERSION        1
#define LINK_ITVL_MIN       12      /* 15 ms: connection interval that the device asks for */
#define LINK_ITVL_MAX       24      /* 30 ms */
#if CONFIG_IDF_TARGET_ESP32
/* The ESP32 controller sleeps through the latency also with reports to
 * send: they go in one burst at the next event that it attends, and the
 * host has buffers for about 16 reports. With 30, a typed text loses keys
 * after 16 reports. With 4, the burst comes each 150 ms, and no buffer
 * runs out: a text types at about 25 ms for each key. The ESP32-S3
 * controller wakes at the next event when it has a report. */
#define LINK_LATENCY        4
#else
#define LINK_LATENCY        30      /* Events the device can miss: wakes about once a second */
#endif
#define LINK_TIMEOUT        600     /* 6 s: more than 2 x (LINK_LATENCY + 1) x LINK_ITVL_MAX */

#define ID(x)               ((void *)(intptr_t)(x))

/* The attributes with an access callback */
enum {
    ATTR_REPORT_MAP,
    ATTR_HID_INFO,
    ATTR_CONTROL_POINT,
    ATTR_PROTOCOL_MODE,
    ATTR_KEYS,
    ATTR_KEYS_REF,
    ATTR_LEDS,
    ATTR_LEDS_REF,
    ATTR_MOUSE,
    ATTR_MOUSE_REF,
    ATTR_BATTERY,
    ATTR_PNP_ID,
};

/* The paired computer. NVS keeps it. */
typedef struct {
    uint8_t version;                    /* 0: no computer */
    ble_addr_t addr;                    /* Identity address */
    char name[BLEHID_NAME_MAX + 1];
} host_t;

static const char *TAG = "blehid";

/* Keyboard: report ID 1, 8 bytes in (modifiers, reserved, 6 keys), 1 byte
 * out (LEDs). Mouse: report ID 2, 3 bytes in (buttons, X, Y). */
static const uint8_t s_report_map[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x06,             /* Usage (Keyboard) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, REPORT_KEYBOARD,  /*   Report ID */
    0x05, 0x07,             /*   Usage Page (Keyboard) */
    0x19, 0xE0,             /*   Usage Minimum (Left Control) */
    0x29, 0xE7,             /*   Usage Maximum (Right GUI) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute): modifiers */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x08,             /*   Report Size (8) */
    0x81, 0x01,             /*   Input (Constant): reserved byte */
    0x95, 0x05,             /*   Report Count (5) */
    0x75, 0x01,             /*   Report Size (1) */
    0x05, 0x08,             /*   Usage Page (LEDs) */
    0x19, 0x01,             /*   Usage Minimum (Num Lock) */
    0x29, 0x05,             /*   Usage Maximum (Kana) */
    0x91, 0x02,             /*   Output (Data, Variable, Absolute): LEDs */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x03,             /*   Report Size (3) */
    0x91, 0x01,             /*   Output (Constant): padding */
    0x95, 0x06,             /*   Report Count (6) */
    0x75, 0x08,             /*   Report Size (8) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x05, 0x07,             /*   Usage Page (Keyboard) */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x2A, 0xFF, 0x00,       /*   Usage Maximum (255) */
    0x81, 0x00,             /*   Input (Data, Array): 6 keys */
    0xC0,                   /* End Collection */
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, REPORT_MOUSE,     /*   Report ID */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x03,             /*     Usage Maximum (3) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute): buttons */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) */
    0x81, 0x01,             /*     Input (Constant): padding */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7F,             /*     Logical Maximum (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative): X, Y */
    0xC0,                   /*   End Collection */
    0xC0,                   /* End Collection */
};

static SemaphoreHandle_t s_lock;        /* s_host, s_pair */
static SemaphoreHandle_t s_adv_lock;    /* update_adv */
static esp_timer_handle_t s_timer;      /* End of a pairing, or of a link without encryption */
static host_t s_host;
static blehid_pair_t s_pair;
static volatile bool s_pairing;
static bool s_discoverable;             /* The advertising that runs is discoverable. */

/* The link. The host task changes it. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_ready;           /* The keys of the paired computer encrypt the link. */
static volatile bool s_notify;          /* The computer takes key reports. */
static volatile uint8_t s_leds;
static uint16_t s_keys_handle;
static uint16_t s_mouse_handle;
static uint16_t s_battery_handle;

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);
static void request_params(uint16_t conn);

/* HID service, Battery service, and Device Information service. The HID
 * attributes need an encrypted link. */
static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_HID),
        .characteristics = (const struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT_MAP),
                .access_cb = gatt_access,
                .arg = ID(ATTR_REPORT_MAP),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_HID_INFO),
                .access_cb = gatt_access,
                .arg = ID(ATTR_HID_INFO),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_CONTROL_POINT),
                .access_cb = gatt_access,
                .arg = ID(ATTR_CONTROL_POINT),
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_PROTOCOL_MODE),
                .access_cb = gatt_access,
                .arg = ID(ATTR_PROTOCOL_MODE),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT),
                .access_cb = gatt_access,
                .arg = ID(ATTR_KEYS),
                .val_handle = &s_keys_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(UUID_REPORT_REF),
                        .access_cb = gatt_access,
                        .arg = ID(ATTR_KEYS_REF),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    },
                    { 0 },
                },
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT),
                .access_cb = gatt_access,
                .arg = ID(ATTR_LEDS),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(UUID_REPORT_REF),
                        .access_cb = gatt_access,
                        .arg = ID(ATTR_LEDS_REF),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    },
                    { 0 },
                },
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID_REPORT),
                .access_cb = gatt_access,
                .arg = ID(ATTR_MOUSE),
                .val_handle = &s_mouse_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(UUID_REPORT_REF),
                        .access_cb = gatt_access,
                        .arg = ID(ATTR_MOUSE_REF),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_BATTERY),
        .characteristics = (const struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_BATTERY_LEVEL),
                .access_cb = gatt_access,
                .arg = ID(ATTR_BATTERY),
                .val_handle = &s_battery_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID_DEVICE_INFO),
        .characteristics = (const struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID_PNP_ID),
                .access_cb = gatt_access,
                .arg = ID(ATTR_PNP_ID),
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Printable ASCII only. */
static void copy_name(char *dst, const uint8_t *src, size_t len)
{
    size_t n = len < BLEHID_NAME_MAX ? len : BLEHID_NAME_MAX;

    for (size_t i = 0; i < n; i++) {
        dst[i] = src[i] >= 0x20 && src[i] <= 0x7E ? src[i] : '?';
    }
    dst[n] = '\0';
}

/* ---- Paired computer in NVS ---- */

static void load_host(void)
{
    nvs_handle_t h;
    size_t size = sizeof(s_host);

    if (nvs_open("blehid", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "host", &s_host, &size) != ESP_OK || size != sizeof(s_host) ||
            s_host.version != HOST_VERSION) {
            memset(&s_host, 0, sizeof(s_host));
        }
        nvs_close(h);
    }
}

static esp_err_t save_host(const host_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("blehid", NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = p->version ? nvs_set_blob(h, "host", p, sizeof(*p)) : nvs_erase_key(h, "host");
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
    s_host = *p;
    unlock();
    return err;
}

static void get_host(host_t *p)
{
    lock();
    *p = s_host;
    unlock();
}

static bool have_host(void)
{
    return s_host.version != 0;
}

/* ---- Advertising ---- */

/* Advertise while no computer is connected: discoverable in a pairing,
 * else only for the paired computer, which knows the address. Any task. */
static void update_adv(void)
{
    struct ble_hs_adv_fields f = { 0 };
    struct ble_gap_adv_params p = { 0 };
    ble_uuid16_t hid = BLE_UUID16_INIT(UUID_HID);
    bool pairing = s_pairing;
    bool want = ble_ready() && s_conn == BLE_HS_CONN_HANDLE_NONE && (pairing || have_host());
    int rc;

    xSemaphoreTake(s_adv_lock, portMAX_DELAY);
    if (ble_gap_adv_active()) {
        if (want && pairing == s_discoverable) {
            xSemaphoreGive(s_adv_lock);
            return;
        }
        ble_gap_adv_stop();
    }
    if (want) {
        f.flags = BLE_HS_ADV_F_BREDR_UNSUP | (pairing ? BLE_HS_ADV_F_DISC_GEN : 0);
        f.appearance = APPEARANCE_KEYBOARD;
        f.appearance_is_present = 1;
        f.uuids16 = &hid;
        f.num_uuids16 = 1;
        f.uuids16_is_complete = 1;
        f.name = (uint8_t *)BLE_NAME;
        f.name_len = strlen(BLE_NAME);
        f.name_is_complete = 1;
        p.conn_mode = BLE_GAP_CONN_MODE_UND;
        p.disc_mode = pairing ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON;
        p.itvl_min = ADV_ITVL_MIN;
        p.itvl_max = ADV_ITVL_MAX;
        rc = ble_gap_adv_set_fields(&f);
        if (rc == 0) {
            rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "advertise: %d", rc);
        } else {
            s_discoverable = pairing;
            ESP_LOGI(TAG, "advertising%s", pairing ? ", discoverable" : "");
        }
    }
    xSemaphoreGive(s_adv_lock);
}

static void arm_timer(uint32_t ms)
{
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, (uint64_t)ms * 1000);
}

/* ---- Pairing ---- */

static void end_pairing(blehid_pair_step_t step, const char *error)
{
    lock();
    s_pairing = false;
    s_pair.step = step;
    s_pair.passkey = 0;
    s_pair.error = error;
    if (step == BLEHID_PAIR_DONE) {
        strlcpy(s_pair.name, s_host.name, sizeof(s_pair.name));
    }
    unlock();
}

/* esp_timer task. A pairing ends without a computer, or a link without
 * encryption ends. */
static void on_timer(void *arg)
{
    uint16_t conn = s_conn;

    if (s_pairing) {
        ESP_LOGW(TAG, "pairing: no computer");
        end_pairing(BLEHID_PAIR_FAILED, "No computer");
        if (conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        update_adv();
    } else if (conn != BLE_HS_CONN_HANDLE_NONE && !s_ready) {
        ESP_LOGW(TAG, "no encryption: the link ends");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* ---- Host task: GAP events ---- */

/* The name of the computer from its GAP service, after the pairing. */
static int on_name(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    uint8_t buf[BLEHID_NAME_MAX];
    uint16_t n;
    host_t h;

    if (error->status != 0 || !attr || OS_MBUF_PKTLEN(attr->om) == 0 || conn != s_conn) {
        return 0;
    }
    n = OS_MBUF_PKTLEN(attr->om) < sizeof(buf) ? OS_MBUF_PKTLEN(attr->om) : sizeof(buf);
    os_mbuf_copydata(attr->om, 0, n, buf);
    get_host(&h);
    if (h.version) {
        copy_name(h.name, buf, n);
        save_host(&h);
        lock();
        strlcpy(s_pair.name, h.name, sizeof(s_pair.name));
        unlock();
        ESP_LOGI(TAG, "computer: %s", h.name);
    }
    return 1;   /* The first value is enough. */
}

/* Passkey entry: the screen shows the passkey, the user types it on the
 * computer. */
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
    s_pair.step = BLEHID_PAIR_PASSKEY;
    unlock();
    ble_sm_inject_io(conn, &io);
}

/* The encryption of the link changed. In a pairing, a link with new
 * authenticated keys makes the computer the paired one. At other times,
 * only the paired computer may stay. */
static void on_encrypted(uint16_t conn, int status)
{
    struct ble_gap_conn_desc desc;
    host_t h = { .version = HOST_VERSION }, old;
    bool found = ble_gap_conn_find(conn, &desc) == 0;
    bool ok = status == 0 && found && desc.sec_state.encrypted && desc.sec_state.bonded &&
              desc.sec_state.authenticated;

    if (conn != s_conn) {
        return;
    }
    if (s_pairing) {
        if (!ok) {
            ESP_LOGW(TAG, "pairing: %d", status);
            if (found && desc.sec_state.bonded) {
                ble_store_util_delete_peer(&desc.peer_id_addr);     /* Keys without a passkey */
            }
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            end_pairing(BLEHID_PAIR_FAILED, "Pairing failed");
            return;
        }
        get_host(&old);
        h.addr = desc.peer_id_addr;
        ble_addr_text(h.name, sizeof(h.name), &h.addr);
        if (old.version && ble_addr_cmp(&old.addr, &h.addr) != 0) {
            ble_store_util_delete_peer(&old.addr);      /* The old computer loses its keys. */
        }
        save_host(&h);
        esp_timer_stop(s_timer);
        s_ready = true;
        end_pairing(BLEHID_PAIR_DONE, NULL);
        ESP_LOGI(TAG, "paired: %s, key size %u", h.name, desc.sec_state.key_size);
        request_params(conn);
        ble_gattc_read_by_uuid(conn, 1, 0xFFFF, BLE_UUID16_DECLARE(UUID_DEVICE_NAME), on_name, NULL);
        return;
    }
    get_host(&old);
    if (ok && old.version && ble_addr_cmp(&desc.peer_id_addr, &old.addr) == 0) {
        esp_timer_stop(s_timer);
        s_ready = true;
        ESP_LOGI(TAG, "connected: %s", old.name);
        request_params(conn);
        return;
    }
    ESP_LOGW(TAG, "unknown link (status %d): ends", status);
    if (found && desc.sec_state.bonded && (!old.version || ble_addr_cmp(&desc.peer_id_addr, &old.addr) != 0)) {
        ble_store_util_delete_peer(&desc.peer_id_addr);     /* Keys of an unknown device */
    }
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            update_adv();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        ESP_LOGI(TAG, "connection");
        if (!s_pairing) {
            /* Ask for the encryption: the paired computer has the keys. An
             * other device gets no keys, and its link ends. */
            ble_gap_security_initiate(s_conn);
            arm_timer(ENCRYPT_MS);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: reason 0x%X", event->disconnect.reason);
        s_ready = false;
        s_notify = false;
        s_leds = 0;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        update_adv();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        on_encrypted(event->enc_change.conn_handle, event->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        on_passkey(event->passkey.conn_handle, event->passkey.params.action);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The computer lost its keys and pairs again: only in a pairing. */
        if (!s_pairing) {
            ESP_LOGW(TAG, "refuse a new pairing");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0 && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "link: interval %u ms, latency %u", (unsigned)(desc.conn_itvl * 5 / 4),
                     (unsigned)desc.conn_latency);
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_keys_handle) {
            s_notify = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "key reports %s", s_notify ? "on" : "off");
        }
        return 0;
    default:
        return 0;
    }
}

/* The host became ready, or it reset. */
static void on_ble(bool ready)
{
    if (ready) {
        ble_svc_gap_device_appearance_set(APPEARANCE_KEYBOARD);
        update_adv();
    } else {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        s_notify = false;
    }
}

/* ---- Host task: GATT server ---- */

static int put(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR && ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* HID 1.11, no country, normally connectable */
    static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x02 };
    /* Vendor ID source USB, then the USB IDs */
    static const uint8_t pnp_id[] = { 0x02, USBDEV_VID & 0xFF, USBDEV_VID >> 8, USBDEV_PID & 0xFF, USBDEV_PID >> 8,
                                      USBDEV_VERSION & 0xFF, USBDEV_VERSION >> 8 };
    static const uint8_t keys_ref[] = { REPORT_KEYBOARD, REPORT_INPUT };
    static const uint8_t leds_ref[] = { REPORT_KEYBOARD, REPORT_OUTPUT };
    static const uint8_t mouse_ref[] = { REPORT_MOUSE, REPORT_INPUT };
    static const uint8_t zero[KEYS_LEN];
    uint8_t v;

    switch ((int)(intptr_t)arg) {
    case ATTR_REPORT_MAP:
        return put(ctxt, s_report_map, sizeof(s_report_map));
    case ATTR_HID_INFO:
        return put(ctxt, hid_info, sizeof(hid_info));
    case ATTR_CONTROL_POINT:
        return 0;   /* Suspend and resume: nothing to do. */
    case ATTR_PROTOCOL_MODE:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return 0;   /* Only the report protocol. */
        }
        v = PROTOCOL_REPORT;
        return put(ctxt, &v, 1);
    case ATTR_KEYS:
        return put(ctxt, zero, KEYS_LEN);
    case ATTR_KEYS_REF:
        return put(ctxt, keys_ref, sizeof(keys_ref));
    case ATTR_LEDS:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (OS_MBUF_PKTLEN(ctxt->om) >= 1 && os_mbuf_copydata(ctxt->om, 0, 1, &v) == 0) {
                s_leds = v;
            }
            return 0;
        }
        v = s_leds;
        return put(ctxt, &v, 1);
    case ATTR_LEDS_REF:
        return put(ctxt, leds_ref, sizeof(leds_ref));
    case ATTR_MOUSE:
        return put(ctxt, zero, MOUSE_LEN);
    case ATTR_MOUSE_REF:
        return put(ctxt, mouse_ref, sizeof(mouse_ref));
    case ATTR_BATTERY:
        v = battery_level() < 0 ? BATTERY_UNKNOWN : battery_level();
        return put(ctxt, &v, 1);
    case ATTR_PNP_ID:
        return put(ctxt, pnp_id, sizeof(pnp_id));
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ---- Reports ---- */

/* A notification with a report. Wait up to wait_ms for a free buffer: a
 * key report must not be lost. The host sends the queued reports in the
 * order of the calls. */
static esp_err_t notify(uint16_t handle, const uint8_t *data, size_t len, int wait_ms)
{
    for (int ms = 0;; ms++) {
        uint16_t conn = s_conn;
        struct os_mbuf *om;
        int rc;

        if (!blehid_ready()) {
            return ESP_ERR_INVALID_STATE;
        }
        om = ble_hs_mbuf_from_flat(data, len);
        rc = om ? ble_gatts_notify_custom(conn, handle, om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            return ESP_OK;
        }
        if (rc != BLE_HS_ENOMEM || ms >= wait_ms) {
            ESP_LOGW(TAG, "report: %d", rc);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t blehid_send_keys(uint8_t mod, const uint8_t keys[6])
{
    uint8_t report[KEYS_LEN] = { mod, 0 };

    memcpy(report + 2, keys, 6);
    return notify(s_keys_handle, report, sizeof(report), SEND_WAIT_MS);
}

esp_err_t blehid_send_mouse(int8_t dx, int8_t dy)
{
    uint8_t report[MOUSE_LEN] = { 0, (uint8_t)dx, (uint8_t)dy };

    return notify(s_mouse_handle, report, sizeof(report), 0);
}

bool blehid_ready(void)
{
    return s_ready && s_notify;
}

bool blehid_caps_lock(void)
{
    return s_leds & LED_CAPS_LOCK;
}

/* The battery level changed (esp_timer task). The computer gets a
 * notification when it asked for them: the host knows that. */
static void on_battery(int level)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_ready) {
        ble_gatts_chr_updated(s_battery_handle);
    }
}

/* Battery: ask the computer for a link with a latency. The device can
 * then miss LINK_LATENCY connection events when it has nothing to send,
 * and the radio wakes about once a second instead of each interval. A
 * report still goes at the next event. The computer can refuse. */
static void request_params(uint16_t conn)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = LINK_ITVL_MIN,
        .itvl_max = LINK_ITVL_MAX,
        .latency = LINK_LATENCY,
        .supervision_timeout = LINK_TIMEOUT,
    };
    int rc = ble_gap_update_params(conn, &params);

    if (rc != 0) {
        ESP_LOGW(TAG, "link parameters: %d", rc);
    }
}

/* ---- API ---- */

esp_err_t blehid_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = on_timer,
        .name = "blehid",
    };

    s_lock = xSemaphoreCreateMutex();
    s_adv_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock && s_adv_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG, "timer");
    load_host();
    ESP_RETURN_ON_ERROR(ble_add_services(s_services), TAG, "services");
    ESP_RETURN_ON_ERROR(ble_on_sync(on_ble), TAG, "sync");
    ESP_RETURN_ON_ERROR(battery_on_change(on_battery), TAG, "battery");
    if (have_host()) {
        ESP_LOGI(TAG, "paired: %s", s_host.name);
        return ble_start();     /* The sync callback starts the advertising. */
    }
    return ESP_OK;
}

blehid_state_t blehid_state(void)
{
    if (!have_host()) {
        return BLEHID_NONE;
    }
    return s_ready ? BLEHID_CONNECTED : BLEHID_WAITING;
}

void blehid_name(char *buf, size_t size)
{
    lock();
    strlcpy(buf, s_host.name, size);
    unlock();
}

esp_err_t blehid_pair_start(void)
{
    esp_err_t err;
    uint16_t conn;

    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    err = ble_start();
    if (err != ESP_OK) {
        return err;
    }
    lock();
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.step = BLEHID_PAIR_WAITING;
    s_pairing = true;
    unlock();
    arm_timer(PAIR_MS);
    conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        /* The new computer replaces the connected one. The disconnect
         * starts the discoverable advertising. */
        s_ready = false;
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        update_adv();
    }
    return ESP_OK;
}

void blehid_pair_status(blehid_pair_t *pair)
{
    lock();
    *pair = s_pair;
    unlock();
}

void blehid_pair_stop(void)
{
    uint16_t conn;

    if (!s_lock || !s_pairing) {
        return;
    }
    end_pairing(BLEHID_PAIR_FAILED, "Stopped");
    esp_timer_stop(s_timer);
    conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE && !s_ready) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    update_adv();
}

esp_err_t blehid_unpair(void)
{
    host_t none = { 0 }, old;
    uint16_t conn;

    if (!s_lock || !have_host()) {
        return ESP_ERR_NOT_FOUND;
    }
    get_host(&old);
    s_ready = false;
    conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (ble_started()) {
        ble_store_util_delete_peer(&old.addr);
    }
    save_host(&none);
    update_adv();
    ESP_LOGI(TAG, "no computer");
    return ESP_OK;
}

#else   /* CONFIG_BT_NIMBLE_ENABLED */

esp_err_t blehid_init(void)
{
    return ESP_OK;
}

blehid_state_t blehid_state(void)
{
    return BLEHID_NONE;
}

void blehid_name(char *buf, size_t size)
{
    if (size > 0) {
        buf[0] = '\0';
    }
}

esp_err_t blehid_pair_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void blehid_pair_status(blehid_pair_t *pair)
{
    *pair = (blehid_pair_t) { .step = BLEHID_PAIR_FAILED, .error = "Not in this build" };
}

void blehid_pair_stop(void)
{
}

esp_err_t blehid_unpair(void)
{
    return ESP_ERR_NOT_FOUND;
}

bool blehid_ready(void)
{
    return false;
}

bool blehid_caps_lock(void)
{
    return false;
}

esp_err_t blehid_send_keys(uint8_t mod, const uint8_t keys[6])
{
    return ESP_ERR_INVALID_STATE;
}

esp_err_t blehid_send_mouse(int8_t dx, int8_t dy)
{
    return ESP_ERR_INVALID_STATE;
}

#endif  /* CONFIG_BT_NIMBLE_ENABLED */
