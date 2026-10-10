/* True random characters. The hardware RNG needs an entropy source: the
 * radio when Wi-Fi runs, else the ADC noise source. All functions are
 * safe to call from different tasks. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bootloader_random.h"
#include "esp_random.h"
#include "vault.h"
#include "rng.h"

static const char s_symbols[] = "!#%+-.:=?@_";
static const char s_password_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "abcdefghijklmnopqrstuvwxyz"
                                     "0123456789!#%+-.:=?@_";

static SemaphoreHandle_t s_lock;
static bool s_radio;

esp_err_t rng_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void rng_radio_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void rng_radio_unlock(bool radio_on)
{
    s_radio = radio_on;
    xSemaphoreGive(s_lock);
}

static void begin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_radio) {
        bootloader_random_enable();
    }
}

static void end(void)
{
    if (!s_radio) {
        bootloader_random_disable();
    }
    xSemaphoreGive(s_lock);
}

static void pick(char *buf, int len, const char *set)
{
    const int n = strlen(set);
    const int limit = 256 - 256 % n;    /* Reject these bytes: no bias. */
    uint8_t rnd[32];

    for (int i = 0; i < len;) {
        esp_fill_random(rnd, sizeof(rnd));
        for (int j = 0; j < (int)sizeof(rnd) && i < len; j++) {
            if (rnd[j] < limit) {
                buf[i++] = set[rnd[j] % n];
            }
        }
    }
    buf[len] = '\0';
    vault_zero(rnd, sizeof(rnd));
}

static bool has_classes(const char *s)
{
    bool upper = false, lower = false, digit = false, symbol = false;

    for (; *s; s++) {
        upper |= isupper((unsigned char)*s) != 0;
        lower |= islower((unsigned char)*s) != 0;
        digit |= isdigit((unsigned char)*s) != 0;
        symbol |= strchr(s_symbols, *s) != NULL;
    }
    return upper && lower && digit && symbol;
}

void rng_text(char *buf, int len, const char *set)
{
    begin();
    pick(buf, len, set);
    end();
}

void rng_password(char *buf, int len)
{
    begin();
    do {
        pick(buf, len, s_password_set);
    } while (!has_classes(buf));
    end();
}
