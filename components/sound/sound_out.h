#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The output of the sound module: speaker.c (the ES8311 codec and the
 * amplifier of the StickS3) or buzzer.c (the passive buzzer of the StickC
 * Plus). The sound task calls these functions, one at a time. */

/* Start the output. After an error, the module makes no sound. */
esp_err_t sound_out_init(void);

/* Turn the output on for a run of sounds. The first note can come at once. */
esp_err_t sound_out_start(void);

/* Turn the output off after a run. */
void sound_out_stop(void);

/* Play one note of ms, or silence if hz is 0. A plain tone keeps its
 * level: the beeps. A bell note rises softly, then falls like the sound
 * of a struck bell: the melody. The stop of the melody (sound_melody_on
 * false) ends a bell note early. */
esp_err_t sound_out_tone(uint32_t hz, uint32_t ms, bool bell);

/* The melody plays: a bell note goes on. From sound.c. */
bool sound_melody_on(void);
