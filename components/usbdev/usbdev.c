/* TinyUSB composite device: HID boot keyboard, CDC-ACM serial port, and
 * HID boot mouse. */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/usb_serial_jtag_ll.h"
#include "nvs.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usbdev.h"

/* 0x4005 is the TinyUSB example PID for CDC + HID. Change the version
 * when the descriptors change: Windows keeps data for each VID, PID, and
 * version. */
#define USB_VID         TINYUSB_ESPRESSIF_VID
#define USB_PID         0x4005
#define USB_BCD         0x0101

/* The ESP32-S3 has 5 IN endpoints, EP0 included. All are in use. */
#define EP_HID_IN       0x81
#define EP_CDC_NOTIF    0x82
#define EP_CDC_OUT      0x03
#define EP_CDC_IN       0x83
#define EP_MOUSE_IN     0x84

#define KEY_DELAY_MS    10      /* After each report. RDP and slow hosts lose fast keys. */
#define CAPS_HOLD_MS    200     /* macOS ignores short Caps Lock pushes. */
#define CAPS_WAIT_MS    500     /* Time for the host to send the new LED state. */
#define LAYOUT_WAIT_MS  150     /* Time for the host to change the key layout */
#define NVS_NAMESPACE   "usb"
#define NVS_LAYOUT      "layout"    /* u16: modifiers in the high byte, key in the low byte */

/* HID usage IDs of the layout key names */
#define KEY_A           0x04
#define KEY_1           0x1E
#define KEY_0           0x27
#define KEY_SPACE       0x2C
#define KEY_F1          0x3A
#define KEY_F13         0x68
#define TX_WAIT_MS      100
#define DFU_DETACH_MS   500     /* Time for the host to see the detach. */
#define RX_BUF          256
#define MOUSE_PERIOD_US (USBDEV_MOUSE_PERIOD_S * 1000000LL)

/* The keyboard is interface 0 for simple BIOS and UEFI drivers. The
 * mouse is the last interface: the serial port keeps its interface
 * number, and Windows keeps its COM port number. */
enum { ITF_HID, ITF_CDC, ITF_CDC_DATA, ITF_MOUSE, ITF_COUNT };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_HID, STR_MOUSE, STR_COUNT };
/* TinyUSB numbers the HID instances in the order of the interfaces. */
enum { HID_KEYBOARD, HID_MOUSE };

static const uint8_t s_keyboard_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };
static const uint8_t s_mouse_report[] = { TUD_HID_REPORT_DESC_MOUSE() };

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_keyboard_report), EP_HID_IN, 8, 1),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, STR_MOUSE, HID_ITF_PROTOCOL_MOUSE, sizeof(s_mouse_report), EP_MOUSE_IN, 8, 10),
};

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static char s_serial[13];

static const char *s_strings[STR_COUNT] = {
    [STR_LANG] = (const char[]) { 0x09, 0x04 },     /* English (US) */
    [STR_MANUFACTURER] = "hw-pass",
    [STR_PRODUCT] = "hw-pass password typer",
    [STR_SERIAL] = s_serial,
    [STR_CDC] = "hw-pass console",
    [STR_HID] = "hw-pass keyboard",
    [STR_MOUSE] = "hw-pass mouse",
};

static const char *TAG = "usbdev";
static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_type_lock;   /* Typing, s_held, and mouse moves */
static uint8_t s_held[6];               /* Keys that usbdev_key holds down */
static bool s_held_lost;                /* The host did not get s_held. */
static volatile uint8_t s_leds;
static volatile bool s_mouse_on;
static int8_t s_mouse_dx = 1;           /* Next move: right, then back to the left */
static uint8_t s_layout_mod;            /* Layout key: modifiers and key, 0: none */
static uint8_t s_layout_key;

static const struct {
    const char *name;
    uint8_t mod;
} s_mods[] = {
    { "Ctrl", KEYBOARD_MODIFIER_LEFTCTRL },
    { "Shift", KEYBOARD_MODIFIER_LEFTSHIFT },
    { "Alt", KEYBOARD_MODIFIER_LEFTALT },
    { "Win", KEYBOARD_MODIFIER_LEFTGUI },
};

/* ---- HID callbacks (TinyUSB task) ---- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == HID_MOUSE ? s_mouse_report : s_keyboard_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buf, uint16_t len)
{
    return 0;
}

/* The host sends the keyboard LED state as an output report. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buf, uint16_t len)
{
    if (instance == HID_KEYBOARD && type == HID_REPORT_TYPE_OUTPUT && len >= 1) {
        s_leds = buf[0];
    }
}

/* ---- Keyboard ---- */

bool usbdev_ready(void)
{
    return tud_mounted() && !tud_suspended();
}

bool usbdev_caps_lock(void)
{
    return s_leds & KEYBOARD_LED_CAPSLOCK;
}

static bool send_keys(uint8_t mod, const uint8_t keys[6])
{
    for (int ms = 0; !tud_hid_ready(); ms++) {
        if (ms >= 200 || !usbdev_ready()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_keyboard_report(0, mod, keys)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_DELAY_MS));
    return true;
}

static bool send_report(uint8_t mod, uint8_t key)
{
    uint8_t keys[6] = { key };

    return send_keys(mod, keys);
}

/* Some hosts need the modifier in an earlier report than the key. */
static bool push_key(uint8_t mod, uint8_t key, int hold_ms)
{
    if (mod && !send_report(mod, 0)) {
        return false;
    }
    if (!send_report(mod, key)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return send_report(0, 0);
}

/* Push Caps Lock, then wait until the host reports the new state.
 * Returns false if the LED state does not change. */
static bool set_caps_lock(bool on)
{
    if (!push_key(0, HID_KEY_CAPS_LOCK, CAPS_HOLD_MS)) {
        return false;
    }
    for (int ms = 0; usbdev_caps_lock() != on; ms += 10) {
        if (ms >= CAPS_WAIT_MS) {
            ESP_LOGW(TAG, "Caps Lock LED did not change");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

esp_err_t usbdev_type(const char *s, bool enter)
{
    static const uint8_t ascii[128][2] = { HID_ASCII_TO_KEYCODE };
    bool caps, ok = true;

    if (!usbdev_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_type_lock, portMAX_DELAY);
    memset(s_held, 0, sizeof(s_held));  /* The next report releases them. */
    s_held_lost = false;
    /* If Caps Lock does not turn off, the host state is unknown. Do not
     * type: the text can have the wrong case. The second push below
     * restores the host state. */
    caps = usbdev_caps_lock();
    if (caps && !set_caps_lock(false)) {
        set_caps_lock(true);
        xSemaphoreGive(s_type_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (s_layout_mod || s_layout_key) {
        ok = push_key(s_layout_mod, s_layout_key, 0);
        vTaskDelay(pdMS_TO_TICKS(LAYOUT_WAIT_MS));
    }
    for (; ok && *s; s++) {
        unsigned char c = *s;
        if (c >= 0x20 && c <= 0x7E) {
            ok = push_key(ascii[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, ascii[c][1], 0);
        }
    }
    if (ok && enter) {
        ok = push_key(0, HID_KEY_ENTER, 0);
    }
    if (!ok) {
        /* A failed push can leave a key down. The host then repeats it. */
        s_held_lost = !send_report(0, 0);
    }
    if (caps) {
        set_caps_lock(true);
    }
    xSemaphoreGive(s_type_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t usbdev_key(uint8_t key, bool down)
{
    uint8_t keys[6] = { 0 };
    bool ok = true;
    int n = 0;

    xSemaphoreTake(s_type_lock, portMAX_DELAY);
    for (int i = 0; i < 6; i++) {
        if (key != 0 && s_held[i] != 0 && s_held[i] != key) {
            keys[n++] = s_held[i];
        }
    }
    if (key != 0 && down && n < 6) {
        keys[n] = key;
    }
    /* Send only a change: the UI releases all keys before each action.
     * After an error, send again: a lost release makes the host repeat. */
    if (memcmp(keys, s_held, sizeof(keys)) != 0 || s_held_lost) {
        memcpy(s_held, keys, sizeof(keys));
        ok = usbdev_ready() && send_keys(0, keys);
        s_held_lost = !ok;
    }
    xSemaphoreGive(s_type_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---- Layout key ---- */

/* Key code of a key name, or 0. */
static uint8_t key_code(const char *name)
{
    size_t n = strlen(name);
    char *end;
    long f;

    if (n == 1 && name[0] >= '1' && name[0] <= '9') {
        return KEY_1 + name[0] - '1';
    }
    if (n == 1 && name[0] == '0') {
        return KEY_0;
    }
    if (n == 1 && isalpha((unsigned char)name[0])) {
        return KEY_A + tolower((unsigned char)name[0]) - 'a';
    }
    if (strcasecmp(name, "space") == 0) {
        return KEY_SPACE;
    }
    if (tolower((unsigned char)name[0]) == 'f' && isdigit((unsigned char)name[1])) {
        f = strtol(name + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 12) {
            return KEY_F1 + f - 1;
        }
        if (*end == '\0' && f >= 13 && f <= 24) {
            return KEY_F13 + f - 13;
        }
    }
    return 0;
}

/* Name of a key code of key_code. */
static void key_name(uint8_t key, char *buf, size_t size)
{
    if (key >= KEY_1 && key < KEY_0) {
        snprintf(buf, size, "%c", '1' + key - KEY_1);
    } else if (key == KEY_0) {
        snprintf(buf, size, "0");
    } else if (key >= KEY_A && key < KEY_A + 26) {
        snprintf(buf, size, "%c", 'A' + key - KEY_A);
    } else if (key == KEY_SPACE) {
        snprintf(buf, size, "Space");
    } else if (key >= KEY_F1 && key < KEY_F1 + 12) {
        snprintf(buf, size, "F%d", key - KEY_F1 + 1);
    } else if (key >= KEY_F13 && key < KEY_F13 + 12) {
        snprintf(buf, size, "F%d", key - KEY_F13 + 13);
    } else {
        snprintf(buf, size, "?");
    }
}

/* "off", "F16", or "ctrl+shift+1": modifiers and at most one key. */
static bool parse_layout(const char *text, uint8_t *mod, uint8_t *key)
{
    char buf[USBDEV_LAYOUT_TEXT * 2], *tok, *save;

    *mod = *key = 0;
    if (strcasecmp(text, "off") == 0) {
        return true;
    }
    if (strlcpy(buf, text, sizeof(buf)) >= sizeof(buf)) {
        return false;
    }
    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        size_t i;

        for (i = 0; i < sizeof(s_mods) / sizeof(s_mods[0]); i++) {
            if (strcasecmp(tok, s_mods[i].name) == 0) {
                *mod |= s_mods[i].mod;
                break;
            }
        }
        if (i < sizeof(s_mods) / sizeof(s_mods[0])) {
            continue;
        }
        if (*key != 0 || (*key = key_code(tok)) == 0) {
            return false;
        }
    }
    return *mod != 0 || *key != 0;
}

esp_err_t usbdev_set_layout(const char *text)
{
    nvs_handle_t h;
    uint8_t mod, key;
    esp_err_t err;

    if (!parse_layout(text, &mod, &key)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, NVS_LAYOUT, (uint16_t)mod << 8 | key);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the layout key: %s", esp_err_to_name(err));
    }
    xSemaphoreTake(s_type_lock, portMAX_DELAY);
    s_layout_mod = mod;
    s_layout_key = key;
    xSemaphoreGive(s_type_lock);
    return ESP_OK;
}

void usbdev_layout_text(char *buf, size_t size)
{
    uint8_t mod = s_layout_mod, key = s_layout_key;
    size_t n = 0;

    if (mod == 0 && key == 0) {
        strlcpy(buf, "off", size);
        return;
    }
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(s_mods) / sizeof(s_mods[0]); i++) {
        if (mod & s_mods[i].mod) {
            n += snprintf(buf + n, size - n, "%s+", s_mods[i].name);
        }
    }
    if (key != 0) {
        key_name(key, buf + n, size - n);
    } else if (n > 0) {
        buf[n - 1] = '\0';  /* Only modifiers: no "+" at the end. */
    }
}

static void load_layout(void)
{
    nvs_handle_t h;
    uint16_t v = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NVS_LAYOUT, &v);
        nvs_close(h);
    }
    s_layout_mod = v >> 8;
    s_layout_key = v & 0xFF;
}

/* ---- Mouse mover ---- */

/* esp_timer task: do not wait. If a move cannot go now, it goes in the
 * next period. No move while the device types: with "focus follows
 * mouse", a move can change the window that gets the keys. */
static void mouse_tick(void *arg)
{
    if (!s_mouse_on || !usbdev_ready() || xSemaphoreTake(s_type_lock, 0) != pdTRUE) {
        return;
    }
    if (tud_hid_n_ready(HID_MOUSE) && tud_hid_n_mouse_report(HID_MOUSE, 0, 0, s_mouse_dx, 0, 0, 0)) {
        s_mouse_dx = -s_mouse_dx;
    }
    xSemaphoreGive(s_type_lock);
}

void usbdev_mouse_mover(bool on)
{
    s_mouse_on = on;
}

bool usbdev_mouse_mover_on(void)
{
    return s_mouse_on;
}

/* ---- Serial port ---- */

static void on_rx(int itf, cdcacm_event_t *event)
{
    uint8_t buf[64];
    size_t n;

    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) == ESP_OK && n > 0) {
        xStreamBufferSend(s_rx, buf, n, 0);
    }
}

bool usbdev_serial_open(void)
{
    return tud_cdc_n_connected(TINYUSB_CDC_ACM_0);
}

size_t usbdev_read(char *buf, size_t size, uint32_t timeout_ms)
{
    return xStreamBufferReceive(s_rx, buf, size, pdMS_TO_TICKS(timeout_ms));
}

static bool put_bytes(const char *p, size_t n, TickType_t wait)
{
    TickType_t start = xTaskGetTickCount();

    while (n > 0) {
        uint32_t k = tud_cdc_n_write(TINYUSB_CDC_ACM_0, p, n);

        p += k;
        n -= k;
        if (n > 0) {
            tud_cdc_n_write_flush(TINYUSB_CDC_ACM_0);
            if (xTaskGetTickCount() - start >= wait) {
                return false;
            }
            vTaskDelay(1);
        }
    }
    return true;
}

static void put(const char *s, size_t n, TickType_t wait)
{
    while (n > 0) {
        const char *nl = memchr(s, '\n', n);
        size_t k = nl ? (size_t)(nl - s) : n;

        if (!put_bytes(s, k, wait) || (nl && !put_bytes("\r\n", 2, wait))) {
            break;
        }
        k += nl ? 1 : 0;
        s += k;
        n -= k;
    }
    tud_cdc_n_write_flush(TINYUSB_CDC_ACM_0);
}

void usbdev_write(const char *s, size_t n)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    put(s, n, pdMS_TO_TICKS(TX_WAIT_MS));
    xSemaphoreGive(s_tx_lock);
}

/* Log output must not block: the TinyUSB task also writes logs, and
 * only that task empties the buffer. */
static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[160];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n > 0 && xSemaphoreTake(s_tx_lock, 0) == pdTRUE) {
        put(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1, 0);
        xSemaphoreGive(s_tx_lock);
    }
    return n;
}

/* ---- Start and download mode ---- */

esp_err_t usbdev_init(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    tinyusb_config_cdcacm_t acm = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = on_rx,
    };
    const esp_timer_create_args_t timer_args = {
        .callback = mouse_tick,
        .name = "mouse",
    };
    esp_timer_handle_t timer;
    uint8_t mac[6];

    s_rx = xStreamBufferCreate(RX_BUF, 1);
    s_tx_lock = xSemaphoreCreateMutex();
    s_type_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rx && s_tx_lock && s_type_lock, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    load_layout();

    cfg.descriptor.device = &s_device;
    cfg.descriptor.full_speed_config = s_config;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STR_COUNT;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "driver");
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm), TAG, "cdc");

    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "%04x:%04x serial %s", USB_VID, USB_PID, s_serial);

    /* After the USB start: the serial port must work also without the timer. */
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, MOUSE_PERIOD_US), TAG, "timer start");
    return ESP_OK;
}

void usbdev_dfu(void)
{
    /* Detach first. Without a gap on the bus, the host does not see the
     * new device and keeps the old one. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(DFU_DETACH_MS));

    /* Give the internal PHY back to USB Serial/JTAG. The ROM download
     * mode uses it. esptool clears the force flag at its hard reset. */
    usb_serial_jtag_ll_phy_enable_external(false);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
