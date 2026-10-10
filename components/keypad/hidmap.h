#pragma once

/* HID keyboard reports, without ESP-IDF parts: the host tests use this file. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_NAME_SIZE   8

/* Position of the key field in a keyboard input report. */
typedef struct {
    uint8_t id;         /* Report ID. 0: the report map has no IDs. */
    uint8_t bitmap;     /* 0: an array of key codes. 1: one bit for each key. */
    uint8_t first;      /* Bitmap: key code of bit 0 */
    uint8_t count;      /* Array: number of codes. Bitmap: number of bits. */
    uint16_t offset;    /* Bit position in the report. The report ID is not part of the report. */
} hid_keys_t;

/* One bit for each key code. */
typedef struct {
    uint8_t bits[32];
} hid_set_t;

/* Find the key field of a keyboard in a HID report map. An array of key
 * codes is better than a bitmap. Return false if the map has no keyboard. */
bool hid_parse_map(const uint8_t *map, size_t len, hid_keys_t *keys);

/* Get the keys that are down. Return false if the report is too short or
 * reports a rollover error (too many keys down). Modifier keys and the
 * codes 0 to 3 are not keys. */
bool hid_keys_down(const hid_keys_t *keys, const uint8_t *report, size_t len, hid_set_t *down);

static inline bool hid_set_has(const hid_set_t *s, uint8_t key)
{
    return s->bits[key / 8] & (1 << (key % 8));
}

/* Short name of a key code for the screen: "5", "Enter", "F1". */
void hid_key_name(uint8_t key, char buf[HID_NAME_SIZE]);
