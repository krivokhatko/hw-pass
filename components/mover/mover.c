/* Mouse mover: the on/off setting, and the schedule that turns the mover
 * on and off at set times. usbdev moves the pointer.
 *
 * The schedule runs in the esp_timer task. Each TICK_MS, it looks at the
 * minutes since the last look and does the changes of these minutes: a
 * change comes at most TICK_MS late, also after a slow period, and also
 * after a small correction of the time forward. When the time becomes
 * known after a power loss, or jumps by JUMP_MIN or more, the mover gets
 * the state of the last change before the new time. After a restart with
 * a known time, the saved state stands. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "clock.h"
#include "sound.h"
#include "usbdev.h"
#include "mover.h"

#define TICK_MS         5000
#define WEEK_MIN        (7 * MOVER_DAY_MIN)
#define JUMP_MIN        (12 * 60)   /* A move of the time by this or more: a new time. */
#define NAMESPACE       "mover"

static const char *TAG = "mover";
static const char *const s_days[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static SemaphoreHandle_t s_lock;    /* s_sched, and the state with its saved copy */
static mover_sched_t s_sched = {
    .enabled = true,
    .on_min = 9 * 60,
    .off_min = 21 * 60,
    .on_days = MOVER_DAYS_ALL,
    .off_days = MOVER_DAYS_ALL,
};
static bool s_armed;                /* s_last is valid. */
static bool s_lost;                 /* The time was not set at a look: a power loss, or the first start. */
static int s_last;                  /* Minute of the week of the last look */

/* ---- Settings in NVS ---- */

static void save_on(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = nvs_set_u8(h, "on", on);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the setting: %s", esp_err_to_name(err));
    }
}

static void save_sched(const mover_sched_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = nvs_set_blob(h, "sched", s, sizeof(*s));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot save the schedule: %s", esp_err_to_name(err));
    }
}

static bool valid_sched(const mover_sched_t *s)
{
    return s->on_min < MOVER_DAY_MIN && s->off_min < MOVER_DAY_MIN &&
           !(s->on_days & ~MOVER_DAYS_ALL) && !(s->off_days & ~MOVER_DAYS_ALL);
}

static void load(void)
{
    nvs_handle_t h;
    mover_sched_t s;
    size_t size = sizeof(s);
    uint8_t on = 0;
    bool got_on = false;

    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        got_on = nvs_get_u8(h, "on", &on) == ESP_OK;
        if (nvs_get_blob(h, "sched", &s, &size) == ESP_OK && size == sizeof(s) && valid_sched(&s)) {
            s_sched = s;
        }
        nvs_close(h);
    }
    /* Firmware without the schedule kept the setting in the "ui" namespace. */
    if (!got_on && nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mouse", &on);
        nvs_close(h);
    }
    usbdev_mouse_mover(on != 0);
}

/* ---- Schedule ---- */

/* Monday 00:00 is 0. tm_wday has Sunday as 0. */
static int week_minute(const struct tm *t)
{
    return ((t->tm_wday + 6) % 7) * MOVER_DAY_MIN + t->tm_hour * 60 + t->tm_min;
}

/* The change of the schedule at this minute of the week, if there is
 * one. With an on and an off at the same minute, off wins: the computer
 * can sleep. */
static bool event_at(const mover_sched_t *s, int m, bool *on)
{
    int day = m / MOVER_DAY_MIN, minute = m % MOVER_DAY_MIN;
    bool found = false;

    if (((s->on_days >> day) & 1) && minute == s->on_min) {
        *on = true;
        found = true;
    }
    if (((s->off_days >> day) & 1) && minute == s->off_min) {
        *on = false;
        found = true;
    }
    return found;
}

/* Change the state and save it, in one step: a change from another task
 * cannot leave a saved state that differs from the state. */
static bool set_state(bool on)
{
    bool changed;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = usbdev_mouse_mover_on() != on;
    if (changed) {
        usbdev_mouse_mover(on);
        save_on(on);
    }
    xSemaphoreGive(s_lock);
    return changed;
}

/* A change by the schedule. */
static void change(bool on)
{
    if (set_state(on)) {
        sound_play(on ? SOUND_ON : SOUND_OFF);
        ESP_LOGI(TAG, "schedule: mouse mover %s", on ? "on" : "off");
    }
}

/* The state of the last change before now, this minute included. */
static void catch_up(const mover_sched_t *s, int now)
{
    bool on;

    if (!s->enabled) {
        return;
    }
    for (int i = 0; i < WEEK_MIN; i++) {
        if (event_at(s, (now - i + WEEK_MIN) % WEEK_MIN, &on)) {
            change(on);
            return;
        }
    }
}

/* esp_timer task. */
static void tick(void *arg)
{
    mover_sched_t s;
    struct tm t;
    int now, steps;
    bool on;

    if (!clock_get(&t)) {
        s_armed = false;
        s_lost = true;
        return;
    }
    now = week_minute(&t);
    mover_get_sched(&s);
    if (!s_armed) {
        /* After a restart, the saved state stands. After a set of a lost
         * time, the mover gets the state of the schedule for this time. */
        if (s_lost) {
            catch_up(&s, now);
        }
        s_armed = true;
        s_lost = false;
        s_last = now;
        return;
    }
    steps = (now - s_last + WEEK_MIN) % WEEK_MIN;
    if (steps > WEEK_MIN - JUMP_MIN) {
        /* The time went back less than JUMP_MIN: wait until it passes
         * s_last. Each change comes one time. */
        return;
    }
    if (steps > JUMP_MIN) {
        catch_up(&s, now);
        s_last = now;
        return;
    }
    while (steps-- > 0) {
        s_last = (s_last + 1) % WEEK_MIN;
        if (s.enabled && event_at(&s, s_last, &on)) {
            change(on);
        }
    }
}

esp_err_t mover_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "sched",
    };
    esp_timer_handle_t timer;

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    load();
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer");
    return esp_timer_start_periodic(timer, TICK_MS * 1000LL);
}

void mover_set(bool on)
{
    set_state(on);
}

bool mover_on(void)
{
    return usbdev_mouse_mover_on();
}

void mover_get_sched(mover_sched_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s = s_sched;
    xSemaphoreGive(s_lock);
}

esp_err_t mover_set_sched(const mover_sched_t *s)
{
    if (!valid_sched(s)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sched = *s;
    save_sched(s);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

int mover_next(bool *on)
{
    mover_sched_t s;
    struct tm t;
    bool now_on = mover_on(), ev;
    int now;

    if (!clock_get(&t)) {
        return -1;
    }
    mover_get_sched(&s);
    if (!s.enabled) {
        return -1;
    }
    now = week_minute(&t);
    /* After a correction back, the changes up to s_last came already. */
    if (s_armed && (now - s_last + WEEK_MIN) % WEEK_MIN > WEEK_MIN - JUMP_MIN) {
        now = s_last;
    }
    for (int i = 1; i <= WEEK_MIN; i++) {
        int m = (now + i) % WEEK_MIN;

        if (event_at(&s, m, &ev) && ev != now_on) {
            *on = ev;
            return m;
        }
    }
    return -1;
}

void mover_next_text(char *buf, size_t size)
{
    struct tm t;
    bool on;
    int m = mover_next(&on), now;

    if (m < 0 || !clock_get(&t)) {
        buf[0] = '\0';
        return;
    }
    now = week_minute(&t);
    if (m / MOVER_DAY_MIN == now / MOVER_DAY_MIN && m > now) {
        snprintf(buf, size, "%s at %02d:%02d", on ? "on" : "off", m % MOVER_DAY_MIN / 60, m % 60);
    } else {
        snprintf(buf, size, "%s %s %02d:%02d", on ? "on" : "off", s_days[m / MOVER_DAY_MIN],
                 m % MOVER_DAY_MIN / 60, m % 60);
    }
}

/* ---- Day masks ---- */

void mover_days_text(uint8_t days, char *buf, size_t size)
{
    size_t n = 0;

    if (days == MOVER_DAYS_ALL) {
        snprintf(buf, size, "every day");
        return;
    }
    if (days == 0) {
        snprintf(buf, size, "never");
        return;
    }
    buf[0] = '\0';
    for (int d = 0; d < 7 && n < size; d++) {
        int end = d;

        if (!((days >> d) & 1)) {
            continue;
        }
        while (end + 1 < 7 && ((days >> (end + 1)) & 1)) {
            end++;
        }
        n += snprintf(buf + n, size - n, "%s%s", n > 0 ? "," : "", s_days[d]);
        if (end > d && n < size) {
            n += snprintf(buf + n, size - n, "-%s", s_days[end]);
        }
        d = end;
    }
}

static int day_index(const char *s)
{
    for (int d = 0; d < 7; d++) {
        if (strncasecmp(s, s_days[d], 3) == 0) {
            return d;
        }
    }
    return -1;
}

bool mover_parse_days(const char *s, uint8_t *days)
{
    uint8_t mask = 0;

    if (strcasecmp(s, "all") == 0) {
        *days = MOVER_DAYS_ALL;
        return true;
    }
    if (strcasecmp(s, "none") == 0) {
        *days = 0;
        return true;
    }
    while (*s) {
        int a = day_index(s), b;

        if (a < 0) {
            return false;
        }
        s += 3;
        b = a;
        if (*s == '-') {
            b = day_index(s + 1);
            if (b < 0) {
                return false;
            }
            s += 4;
        }
        for (int d = a;; d = (d + 1) % 7) {
            mask |= 1 << d;
            if (d == b) {
                break;
            }
        }
        if (*s == ',') {
            s++;
        } else if (*s != '\0') {
            return false;
        }
    }
    if (mask == 0) {
        return false;
    }
    *days = mask;
    return true;
}
