/* TinyUSB composite device: HID boot keyboard, CDC-ACM serial port, and
 * HID boot mouse. hid.c makes the key and mouse reports. */
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

/* The ESP32-S3 has 5 IN endpoints, EP0 included. All are in use. */
#define EP_HID_IN       0x81
#define EP_CDC_NOTIF    0x82
#define EP_CDC_OUT      0x03
#define EP_CDC_IN       0x83
#define EP_MOUSE_IN     0x84

#define SEND_WAIT_MS    200     /* Time for a free keyboard endpoint */
#define TX_WAIT_MS      100
#define DFU_DETACH_MS   500     /* Time for the host to see the detach. */
#define RX_BUF          256

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
    .idVendor = USBDEV_VID,
    .idProduct = USBDEV_PID,
    .bcdDevice = USBDEV_VERSION,
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
static volatile uint8_t s_leds;

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

/* ---- Keyboard and mouse ---- */

bool usbdev_ready(void)
{
    return tud_mounted() && !tud_suspended();
}

bool usbdev_caps_lock(void)
{
    return s_leds & KEYBOARD_LED_CAPSLOCK;
}

bool usbdev_send_keys(uint8_t mod, const uint8_t keys[6])
{
    uint8_t copy[6];

    for (int ms = 0; !tud_hid_ready(); ms++) {
        if (ms >= SEND_WAIT_MS || !usbdev_ready()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    memcpy(copy, keys, sizeof(copy));   /* TinyUSB takes a non-const array. */
    return tud_hid_keyboard_report(0, mod, copy);
}

bool usbdev_send_mouse(int8_t dx, int8_t dy)
{
    return usbdev_ready() && tud_hid_n_ready(HID_MOUSE) && tud_hid_n_mouse_report(HID_MOUSE, 0, 0, dx, dy, 0, 0);
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
    ESP_RETURN_ON_FALSE(s_rx && s_tx_lock, ESP_ERR_NO_MEM, TAG, "no memory");

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
    ESP_LOGI(TAG, "%04x:%04x serial %s", USBDEV_VID, USBDEV_PID, s_serial);
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
