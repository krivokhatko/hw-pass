#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Time of the device. The system clock has UTC. A time zone gives the
 * local time, also with daylight saving time. The ESP32-S3 keeps the
 * time across a restart, but not across a power loss: then the time is
 * not set until the user or the time sync sets it again. All functions
 * are safe to call from different tasks. */

#define CLOCK_MIN_YEAR  2024    /* An earlier time means: the time is not set. */
#define CLOCK_TZ_MAX    48      /* Characters of a zone */

/* Load the time zone from NVS. Call before the other functions. */
esp_err_t clock_init(void);

/* The time is set. */
bool clock_valid(void);

/* Copy the local time, with the weekday. Return false if the time is not
 * set: then t has the time since the start, in 1970. */
bool clock_get(struct tm *t);

/* Set the local time. tm_wday and tm_isdst can be wrong: they are
 * computed. ESP_ERR_INVALID_ARG: not a date, or before CLOCK_MIN_YEAR. */
esp_err_t clock_set(const struct tm *t);

/* Set the time in UTC. ESP_ERR_INVALID_ARG: before CLOCK_MIN_YEAR, or
 * after 2099. */
esp_err_t clock_set_utc(const struct timeval *tv);

/* Parse "YYYY-MM-DD HH:MM" into t, with the weekday and 0 seconds.
 * Spaces at the end are allowed. ESP_ERR_INVALID_ARG: another form, a
 * date that does not exist, or a year before CLOCK_MIN_YEAR. */
esp_err_t clock_parse(const char *s, struct tm *t);

/* The time zone, as a POSIX TZ string: the name and the offset of the
 * standard time, then the name of the daylight saving time and its
 * rules, for example "EET-2EEST,M3.5.0/3,M10.5.0/4". The default is
 * "UTC0". The device keeps the zone. ESP_ERR_INVALID_ARG: the C library
 * cannot parse it, or it is longer than CLOCK_TZ_MAX. */
esp_err_t clock_set_tz(const char *tz);
void clock_get_tz(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
