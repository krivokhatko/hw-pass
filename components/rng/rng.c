/* True random characters. The hardware RNG needs an entropy source: the
 * radio when Wi-Fi or Bluetooth runs, else the ADC noise source. All
 * functions are safe to call from different tasks. */
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bootloader_random.h"
#include "esp_random.h"
#include "vault.h"
#include "rng.h"

/* Symbols without a special function in bash, zsh, cmd, and PowerShell,
 * when they are not the first character. */
static const char s_symbols[] = "+-.:@_";
static const char s_password_set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "abcdefghijklmnopqrstuvwxyz"
                                     "0123456789+-.:@_";

static SemaphoreHandle_t s_lock;
static unsigned s_radio;  /* rng_radio_t bits of the radios that run */

esp_err_t rng_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void rng_radio_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void rng_radio_unlock(rng_radio_t radio, bool on)
{
    if (on) {
        s_radio |= radio;
    } else {
        s_radio &= ~radio;
    }
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

/* True if s has all classes, and a letter or digit first. A symbol
 * first can be an option (-x), a splat (@x), or a response file (@x). */
static bool good(const char *s)
{
    bool upper = false, lower = false, digit = false, symbol = false;

    if (!isalnum((unsigned char)s[0])) {
        return false;
    }
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

void rng_bytes(void *buf, size_t len)
{
    begin();
    esp_fill_random(buf, len);
    end();
}

/* A rejected password is not used: all accepted passwords have the same
 * probability. */
void rng_password(char *buf, int len)
{
    begin();
    do {
        pick(buf, len, s_password_set);
    } while (!good(buf));
    end();
}
