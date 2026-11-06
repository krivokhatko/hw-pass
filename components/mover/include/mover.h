#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse mover: while it is on, the device moves the mouse pointer of the
 * computer 1 pixel each 30 s, and the computer does not lock or sleep.
 * hid moves the pointer, over USB or Bluetooth. This module keeps the
 * on/off setting, and turns the mover on and off at set times: the
 * schedule. All functions are safe to call from different tasks. */

#define MOVER_DAY_MIN       1440    /* Minutes in a day */
#define MOVER_DAYS_ALL      0x7F
#define MOVER_DAYS_TEXT     24      /* Buffer size for mover_days_text */
#define MOVER_NEXT_TEXT     16      /* Buffer size for mover_next_text */

/* The schedule. Bit i of a day mask is a weekday: bit 0 Monday, bit 6
 * Sunday. At on_min on a day in on_days, the mover goes on. At off_min on
 * a day in off_days, it goes off. Between the times, the user can turn
 * the mover on or off: the state stays until the next time. */
typedef struct {
    bool enabled;
    uint16_t on_min;        /* Minute of the day, 0..1439 */
    uint16_t off_min;
    uint8_t on_days;
    uint8_t off_days;
} mover_sched_t;

/* Load the setting and the schedule, and start the schedule. Call after
 * hid_init. The schedule does a change up to 5 s after its time.
 * When the time becomes known after a power loss, or moves by 12 h or
 * more, the mover gets the state of the last change before the new time.
 * A small correction of the time forward does the changes that it skips;
 * a small correction back does no change two times. */
esp_err_t mover_init(void);

/* Turn the mover on or off, and keep the setting. */
void mover_set(bool on);
bool mover_on(void);

void mover_get_sched(mover_sched_t *s);

/* Set the schedule and keep it. ESP_ERR_INVALID_ARG: a time is not
 * 0..1439, or a day mask has bits above bit 6. */
esp_err_t mover_set_sched(const mover_sched_t *s);

/* The next change of the state by the schedule: the next time with a
 * state that differs from the current state. Set *on to that state and
 * return the minute of the week, 0 is Monday 00:00. Return -1 if the
 * schedule is off, the time is not set, or no change comes. */
int mover_next(bool *on);

/* The next change as text: "off at 21:00" today, "off Mon 21:00" on
 * another day, "" if no change comes. */
void mover_next_text(char *buf, size_t size);

/* A day mask as text: "Mon-Fri", "Mon,Wed,Sat-Sun", "every day", "never". */
void mover_days_text(uint8_t days, char *buf, size_t size);

/* Parse a day mask: "all", "none", or names of 3 letters in any case,
 * in a list with commas or a range with a dash: "mon,wed", "mon-fri",
 * "fri-mon". Return false if the text has another form. */
bool mover_parse_days(const char *s, uint8_t *days);

#ifdef __cplusplus
}
#endif
