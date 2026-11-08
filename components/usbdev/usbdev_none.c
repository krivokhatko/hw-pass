/* A chip without USB (ESP32): no USB device. The keys go to a computer
 * over Bluetooth LE only, and the console is UART0 (components/console).
 * The functions say that no host is there. */
#include "esp_system.h"
#include "usbdev.h"

esp_err_t usbdev_init(void)
{
    return ESP_OK;
}

bool usbdev_ready(void)
{
    return false;
}

bool usbdev_caps_lock(void)
{
    return false;
}

bool usbdev_send_keys(uint8_t mod, const uint8_t keys[6])
{
    return false;
}

bool usbdev_send_mouse(int8_t dx, int8_t dy)
{
    return false;
}

bool usbdev_serial_open(void)
{
    return false;
}

size_t usbdev_read(char *buf, size_t size, uint32_t timeout_ms)
{
    return 0;
}

void usbdev_write(const char *s, size_t n)
{
}

/* The ESP32 has no download mode by software. A restart: esptool resets
 * the chip into the download mode through the USB bridge of the board. */
void usbdev_dfu(void)
{
    esp_restart();
}
