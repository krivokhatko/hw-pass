/* The serial port. With USB, usbdev has the port: this module passes
 * the calls on. Without USB, UART0 is the port: the UART driver with a
 * send buffer, and the log output goes through the same buffer, so that
 * the log lines and the command line do not mix. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "serial.h"

#if CONFIG_HWPASS_USB
#include "usbdev.h"

esp_err_t serial_init(void)
{
    return ESP_OK;
}

bool serial_open(void)
{
    return usbdev_serial_open();
}

size_t serial_read(char *buf, size_t size, uint32_t timeout_ms)
{
    return usbdev_read(buf, size, timeout_ms);
}

void serial_write(const char *s, size_t n)
{
    usbdev_write(s, n);
}

#else   /* UART0 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"

#define UART        UART_NUM_0  /* The console UART of ESP-IDF: the ROM set its baud rate for the start-up log. */
#define RX_BUF      256
#define TX_BUF      1024

static const char *TAG = "serial";
static SemaphoreHandle_t s_lock;

/* Write with CR LF line ends. Waits for space in the send buffer. */
static void put(const char *s, size_t n)
{
    while (n > 0) {
        const char *nl = memchr(s, '\n', n);
        size_t k = nl ? (size_t)(nl - s) : n;

        uart_write_bytes(UART, s, k);
        if (nl) {
            uart_write_bytes(UART, "\r\n", 2);
            k++;
        }
        s += k;
        n -= k;
    }
}

/* Log output: no wait for the lock. A line that comes while the command
 * line writes is lost, not mixed into its text. */
static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[160];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (n > 0 && xSemaphoreTake(s_lock, 0) == pdTRUE) {
        put(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
        xSemaphoreGive(s_lock);
    }
    return n;
}

esp_err_t serial_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART, RX_BUF, TX_BUF, 0, NULL, 0), TAG, "driver");
    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "UART%d", UART);
    return ESP_OK;
}

bool serial_open(void)
{
    return true;
}

size_t serial_read(char *buf, size_t size, uint32_t timeout_ms)
{
    int n = uart_read_bytes(UART, buf, size, pdMS_TO_TICKS(timeout_ms));

    return n > 0 ? (size_t)n : 0;
}

void serial_write(const char *s, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    put(s, n);
    xSemaphoreGive(s_lock);
}

#endif  /* CONFIG_HWPASS_USB */
