#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Short sounds on the speaker. The sound task plays them: the caller
 * does not wait. All functions are safe to call from different tasks. */

typedef enum {
    SOUND_CLICK,    /* The selection changed. */
    SOUND_STEP,     /* A hold reached the next step, or the user must act. */
    SOUND_DONE,     /* The password is typed. */
    SOUND_OK,       /* An action is complete: a key has an entry, a keypad is paired. */
    SOUND_ON,       /* A function is on. */
    SOUND_OFF,      /* A function is off, or a key lost its entry. */
    SOUND_ERROR,
    SOUND_COUNT,
} sound_t;

/* Start the ES8311 codec and the sound task. Call after pm1_init. The
 * sounds are off until sound_enable(true). */
esp_err_t sound_init(void);

/* Play a sound after the sounds that wait. Does nothing when the sounds
 * are off. If too many sounds wait, the new sound is lost. */
void sound_play(sound_t s);

void sound_enable(bool on);
bool sound_enabled(void);

/* The codec started: the speaker works. */
bool sound_ready(void);

#ifdef __cplusplus
}
#endif
