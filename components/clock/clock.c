/* Time of the device. The system clock has UTC. The ESP32-S3 keeps it
 * across a restart, but not across a power loss. The StickS3 has no clock
 * chip: nothing answers at the I2C address of the BM8563 (0x51) of the
 * older M5Stack boards.
 *
 * The time zone is the TZ string of the C library. localtime and mktime
 * use it, also for daylight saving time. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "clock.h"

#define NAMESPACE   "clock"
#define TZ_DEFAULT  "UTC0"

static const char *TAG = "clock";
static char s_tz[CLOCK_TZ_MAX + 1] = TZ_DEFAULT;

/* Give the zone to the C library. Return false if the library cannot
 * parse it: then the name of the standard time is empty. */
static bool apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    return tzname[0] != NULL && tzname[0][0] != '\0';
}

/* The fields are in range, and the year is CLOCK_MIN_YEAR to 2099. */
static bool valid_fields(const struct tm *t)
{
    return t->tm_year >= CLOCK_MIN_YEAR - 1900 && t->tm_year <= 2099 - 1900 &&
           t->tm_mon >= 0 && t->tm_mon <= 11 && t->tm_mday >= 1 && t->tm_mday <= 31 &&
           t->tm_hour >= 0 && t->tm_hour <= 23 && t->tm_min >= 0 && t->tm_min <= 59 &&
           t->tm_sec >= 0 && t->tm_sec <= 59;
}

/* Compute the weekday, and if daylight saving time applies. secs gets
 * the time in UTC. Return false if the date does not exist, for example
 * February 30: mktime moves it to the next month. */
static bool complete(struct tm *t, time_t *secs)
{
    struct tm copy = *t;
    time_t s;

    copy.tm_isdst = -1;     /* mktime finds out if daylight saving time applies. */
    s = mktime(&copy);
    if (s == (time_t)-1 || copy.tm_mday != t->tm_mday) {
        return false;
    }
    *t = copy;
    if (secs) {
        *secs = s;
    }
    return true;
}

esp_err_t clock_init(void)
{
    nvs_handle_t h;
    char tz[CLOCK_TZ_MAX + 1];
    size_t size = sizeof(tz);
    bool ok = false;

    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_str(h, "tz", tz, &size) == ESP_OK && apply_tz(tz);
        nvs_close(h);
    }
    if (ok) {
        strcpy(s_tz, tz);
    } else {
        apply_tz(TZ_DEFAULT);
    }
    return ESP_OK;
}

bool clock_get(struct tm *t)
{
    time_t now = time(NULL);

    localtime_r(&now, t);
    return t->tm_year >= CLOCK_MIN_YEAR - 1900;
}

bool clock_valid(void)
{
    struct tm t;

    return clock_get(&t);
}

esp_err_t clock_set(const struct tm *t)
{
    struct tm copy = *t;
    struct timeval tv = { 0 };

    if (!valid_fields(&copy) || !complete(&copy, &tv.tv_sec)) {
        return ESP_ERR_INVALID_ARG;
    }
    settimeofday(&tv, NULL);
    return ESP_OK;
}

esp_err_t clock_set_utc(const struct timeval *tv)
{
    time_t secs = tv->tv_sec;
    struct tm t;

    gmtime_r(&secs, &t);
    if (t.tm_year < CLOCK_MIN_YEAR - 1900 || t.tm_year > 2099 - 1900) {
        return ESP_ERR_INVALID_ARG;
    }
    settimeofday(tv, NULL);
    return ESP_OK;
}

esp_err_t clock_parse(const char *s, struct tm *t)
{
    int y, mo, d, h, mi, n = 0;

    if (sscanf(s, "%4d-%2d-%2d %2d:%2d%n", &y, &mo, &d, &h, &mi, &n) != 5) {
        return ESP_ERR_INVALID_ARG;
    }
    while (s[n] == ' ') {
        n++;
    }
    if (s[n] != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    memset(t, 0, sizeof(*t));
    t->tm_year = y - 1900;
    t->tm_mon = mo - 1;
    t->tm_mday = d;
    t->tm_hour = h;
    t->tm_min = mi;
    return valid_fields(t) && complete(t, NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t clock_set_tz(const char *tz)
{
    size_t len = strlen(tz);
    nvs_handle_t h;
    esp_err_t err;

    if (len == 0 || len > CLOCK_TZ_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isgraph((unsigned char)tz[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!apply_tz(tz)) {
        apply_tz(s_tz);
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(s_tz, tz);
    err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "tz", tz);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the time zone: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

void clock_get_tz(char *buf, size_t size)
{
    snprintf(buf, size, "%s", s_tz);
}
