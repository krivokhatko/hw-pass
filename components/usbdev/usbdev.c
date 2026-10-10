/* TinyUSB composite device: HID boot keyboard and CDC-ACM serial port. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usbdev.h"

/* 0x4005 is the TinyUSB example PID for CDC + HID. */
#define USB_VID         TINYUSB_ESPRESSIF_VID
#define USB_PID         0x4005
#define USB_BCD         0x0100

#define EP_HID_IN       0x81
#define EP_CDC_NOTIF    0x82
#define EP_CDC_OUT      0x03
#define EP_CDC_IN       0x83

#define KEY_DELAY_MS    10      /* After each report. RDP and slow hosts lose fast keys. */
#define CAPS_HOLD_MS    200     /* macOS ignores short Caps Lock pushes. */
#define CAPS_WAIT_MS    500     /* Time for the host to send the new LED state. */
#define TX_WAIT_MS      100
#define DFU_DETACH_MS   500     /* Time for the host to see the detach. */
#define RX_BUF          256

/* The keyboard is interface 0 for simple BIOS and UEFI drivers. */
enum { ITF_HID, ITF_CDC, ITF_CDC_DATA, ITF_COUNT };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_HID, STR_COUNT };

static const uint8_t s_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report), EP_HID_IN, 8, 1),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
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
};

static const char *TAG = "usbdev";
static StreamBufferHandle_t s_rx;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_type_lock;
static volatile uint8_t s_leds;

/* ---- HID callbacks (TinyUSB task) ---- */

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return s_hid_report;
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
    if (type == HID_REPORT_TYPE_OUTPUT && len >= 1) {
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

static bool send_report(uint8_t mod, uint8_t key)
{
    uint8_t keys[6] = { key };

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
    /* If Caps Lock does not turn off, the host state is unknown. Do not
     * type: the text can have the wrong case. The second push below
     * restores the host state. */
    caps = usbdev_caps_lock();
    if (caps && !set_caps_lock(false)) {
        set_caps_lock(true);
        xSemaphoreGive(s_type_lock);
        return ESP_ERR_INVALID_RESPONSE;
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
        send_report(0, 0);
    }
    if (caps) {
        set_caps_lock(true);
    }
    xSemaphoreGive(s_type_lock);
    return ok ? ESP_OK : ESP_FAIL;
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
    uint8_t mac[6];

    s_rx = xStreamBufferCreate(RX_BUF, 1);
    s_tx_lock = xSemaphoreCreateMutex();
    s_type_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rx && s_tx_lock && s_type_lock, ESP_ERR_NO_MEM, TAG, "no memory");

    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cfg.descriptor.device = &s_device;
    cfg.descriptor.full_speed_config = s_config;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STR_COUNT;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "driver");
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm), TAG, "cdc");

    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "%04x:%04x serial %s", USB_VID, USB_PID, s_serial);
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
