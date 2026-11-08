/* Short sounds and the typing melody. The sound task plays them on the
 * output of the board: the speaker (speaker.c) or the buzzer (buzzer.c),
 * see sound_out.h. */
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "sound.h"

#if CONFIG_HWPASS_SOUND
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sound_out.h"

#define START_TRIES     2       /* Attempts to turn on the output for one sound */
#define START_RETRY_MS  20
#define PAD_MS          10      /* Silence before a sound after an idle time (see sound_task) */
#define IDLE_MS         1000    /* The output stays on for the next sound, also for a long push (0.6 s). */
#define QUEUE_LEN       8
#define PREPARE         SOUND_COUNT     /* Queue item: turn on the output, no sound */
#define MELODY          (SOUND_COUNT + 1)   /* Queue item: the melody of s_melody_seed */

/* The melody: a run of bell notes through the scale, until the stop. */
#define MELODY_MAX      300     /* Safety limit when no stop comes: about 30 s */
#define NOTE_MS         90      /* The shortest note. The seed adds 0 to 3 steps. */
#define NOTE_STEP_MS    20
#define NOTE_GAP_MS     15      /* Rest before the next note: the notes stay apart. */

/* Notes in Hz. The small speaker is weak below about 700 Hz. */
#define G5  784
#define C6  1047
#define D6  1175
#define E6  1319
#define G6  1568
#define A6  1760
#define C7  2093
#define D7  2349
#define E7  2637

typedef struct {
    uint16_t hz;    /* 0: silence */
    uint16_t ms;    /* 0: end of the sound */
} note_t;

static const note_t s_click[] = { { E7, 6 }, { 0, 0 } };
static const note_t s_step[] = { { A6, 50 }, { 0, 0 } };
static const note_t s_ok[] = { { E6, 60 }, { G6, 60 }, { C7, 120 }, { 0, 0 } };
static const note_t s_on[] = { { C6, 70 }, { 0, 20 }, { G6, 100 }, { 0, 0 } };
static const note_t s_off[] = { { G6, 70 }, { 0, 20 }, { C6, 100 }, { 0, 0 } };
static const note_t s_error[] = { { G5, 100 }, { 0, 60 }, { G5, 180 }, { 0, 0 } };

static const note_t *const s_sounds[SOUND_COUNT] = {
    [SOUND_CLICK] = s_click,
    [SOUND_STEP] = s_step,
    [SOUND_OK] = s_ok,
    [SOUND_ON] = s_on,
    [SOUND_OFF] = s_off,
    [SOUND_ERROR] = s_error,
};

/* The scale of the melody: C major pentatonic, no two notes of it clash. */
static const uint16_t s_scale[8] = { C6, D6, E6, G6, A6, C7, D7, E7 };

static const char *TAG = "sound";
static QueueHandle_t s_queue;
static volatile bool s_enabled;
static bool s_ready;
static volatile uint32_t s_melody_seed;
static volatile bool s_melody_on;

bool sound_melody_on(void)
{
    return s_melody_on;
}

static esp_err_t tone(uint32_t hz, uint32_t ms)
{
    return sound_out_tone(hz, ms, false);
}

static esp_err_t note(uint32_t hz, uint32_t ms)
{
    return sound_out_tone(hz, ms, true);
}

/* A run through the scale, up and down, until the stop. The seed gives
 * the first note, the step of 1 to 3 degrees, the direction, and the
 * speed. At the ends of the scale, the run turns. The stop ends the note
 * that plays: the melody is not longer than the typing. */
static void melody(uint32_t seed)
{
    int d = seed & 7;
    int step = 1 + (seed >> 3) % 3;
    int dir = (seed >> 5) & 1 ? 1 : -1;
    uint32_t ms = NOTE_MS + NOTE_STEP_MS * ((seed >> 6) & 3);

    for (int i = 0; i < MELODY_MAX && s_melody_on; i++) {
        if (note(s_scale[d], ms) != ESP_OK || tone(0, NOTE_GAP_MS) != ESP_OK) {
            return;
        }
        d += dir * step;
        if (d < 0) {
            d = -d;
            dir = 1;
        }
        if (d > 7) {
            d = 14 - d;
            dir = -1;
        }
    }
}

static void play(uint8_t s)
{
    if (s == MELODY) {
        melody(s_melody_seed);
    } else if (s < SOUND_COUNT) {
        for (const note_t *n = s_sounds[s]; n->ms > 0; n++) {
            if (tone(n->hz, n->ms) != ESP_OK) {
                return;
            }
        }
    }
    /* PREPARE: nothing */
}

/* Turn on the output for a sound. One failed start does not lose the
 * sound: a second attempt comes after a short time. */
static bool start_retry(void)
{
    for (int i = 0; i < START_TRIES; i++) {
        if (sound_out_start() == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(START_RETRY_MS));
    }
    ESP_LOGW(TAG, "the output does not start: sound lost");
    return false;
}

/* Play the sounds of the queue. The output stays on while the next sound
 * or PREPARE comes in less than IDLE_MS. After this time, the output goes
 * off. A sound after an idle time on an output that is on starts with
 * one buffer of silence: the speaker needs it (speaker.c), and if this
 * write fails, the output is in a bad state and a new start makes it
 * good again. */
static void sound_task(void *arg)
{
    uint8_t s;
    bool on = false;    /* The output is on. */

    for (;;) {
        if (xQueueReceive(s_queue, &s, on ? pdMS_TO_TICKS(IDLE_MS) : portMAX_DELAY) != pdTRUE) {
            sound_out_stop();
            on = false;
            continue;
        }
        if (on && s != PREPARE && tone(0, PAD_MS) != ESP_OK) {
            sound_out_stop();
            on = false;
        }
        if (!on) {
            on = start_retry();
        }
        if (on) {
            play(s);
        }
    }
}

esp_err_t sound_init(void)
{
    ESP_RETURN_ON_ERROR(sound_out_init(), TAG, "output");
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(uint8_t));
    if (s_queue == NULL || xTaskCreate(sound_task, "sound", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    return ESP_OK;
}

static void send(uint8_t v)
{
    if (s_ready && s_enabled && xQueueSend(s_queue, &v, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full: sound %u lost", v);
    }
}

void sound_play(sound_t s)
{
    if (s < SOUND_COUNT) {
        send(s);
    }
}

void sound_prepare(void)
{
    send(PREPARE);
}

/* FNV-1a: the same text gives the same seed. */
static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;

    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

void sound_melody_start(const char *text)
{
    s_melody_seed = hash(text);
    s_melody_on = true;
    send(MELODY);
}

void sound_melody_stop(void)
{
    s_melody_on = false;
}

void sound_enable(bool on)
{
    s_enabled = on;
}

bool sound_enabled(void)
{
    return s_enabled;
}

bool sound_ready(void)
{
    return s_ready;
}

#else   /* CONFIG_HWPASS_SOUND */

/* A board without a speaker and without a buzzer: the sounds go nowhere.
 * The LEDs of the board show the state instead (components/ui/view_led.c). */
esp_err_t sound_init(void)
{
    return ESP_OK;
}

void sound_play(sound_t s)
{
}

void sound_prepare(void)
{
}

void sound_melody_start(const char *text)
{
}

void sound_melody_stop(void)
{
}

void sound_enable(bool on)
{
}

bool sound_enabled(void)
{
    return false;
}

bool sound_ready(void)
{
    return false;
}

#endif  /* CONFIG_HWPASS_SOUND */
