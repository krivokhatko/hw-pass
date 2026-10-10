/* HID keyboard reports: the report map parser, the keys that are down, and
 * the key names. Usage IDs are from the HID Usage Tables, page 0x07. */
#include <stdio.h>
#include <string.h>
#include "hidmap.h"

#define PAGE_KEYBOARD   0x07
#define KEY_FIRST       0x04    /* 0 to 3 are "no key" and error codes. */
#define KEY_ROLLOVER    0x01
#define KEY_MODIFIER    0xE0    /* 0xE0 to 0xE7: Ctrl, Shift, Alt, GUI */

/* Item prefixes without the size bits. */
#define ITEM_INPUT      0x80
#define ITEM_OUTPUT     0x90
#define ITEM_COLLECTION 0xA0
#define ITEM_FEATURE    0xB0
#define ITEM_END        0xC0
#define ITEM_PAGE       0x04
#define ITEM_SIZE       0x74
#define ITEM_ID         0x84
#define ITEM_COUNT      0x94
#define ITEM_USAGE_MIN  0x18
#define ITEM_LONG       0xFE

#define INPUT_CONSTANT  0x01
#define INPUT_VARIABLE  0x02

bool hid_parse_map(const uint8_t *map, size_t len, hid_keys_t *keys)
{
    uint16_t bits[256] = { 0 };     /* Input bits of each report ID so far */
    uint32_t page = 0, size = 0, count = 0, id = 0, usage_min = 0;
    hid_keys_t bitmap = { 0 };
    bool have_bitmap = false;

    for (size_t i = 0; i < len;) {
        uint8_t prefix = map[i++];
        size_t n = (prefix & 3) == 3 ? 4 : prefix & 3;
        uint32_t v = 0;

        if (prefix == ITEM_LONG) {
            if (i >= len) {
                break;
            }
            i += 2 + map[i];    /* Data size, tag, data */
            continue;
        }
        if (n > len - i) {
            break;
        }
        for (size_t j = 0; j < n; j++) {
            v |= (uint32_t)map[i + j] << (8 * j);
        }
        i += n;
        switch (prefix & 0xFC) {
        case ITEM_PAGE:
            page = v;
            break;
        case ITEM_SIZE:
            size = v;
            break;
        case ITEM_ID:
            id = v & 0xFF;
            break;
        case ITEM_COUNT:
            count = v;
            break;
        case ITEM_USAGE_MIN:
            usage_min = v;
            break;
        case ITEM_INPUT:
            if (!(v & INPUT_CONSTANT) && page == PAGE_KEYBOARD && count > 0 && count < 256 &&
                bits[id] < 0xFF00) {
                if (!(v & INPUT_VARIABLE) && size == 8 && bits[id] % 8 == 0) {
                    *keys = (hid_keys_t) { .id = id, .count = count, .offset = bits[id] };
                    return true;
                }
                if ((v & INPUT_VARIABLE) && size == 1 && usage_min < KEY_MODIFIER && !have_bitmap) {
                    bitmap = (hid_keys_t) { .id = id, .bitmap = 1, .first = usage_min, .count = count, .offset = bits[id] };
                    have_bitmap = true;
                }
            }
            bits[id] += size * count;
            usage_min = 0;  /* Local items end at a main item. */
            break;
        case ITEM_OUTPUT:
        case ITEM_COLLECTION:
        case ITEM_FEATURE:
        case ITEM_END:
            usage_min = 0;
            break;
        }
    }
    *keys = bitmap;
    return have_bitmap;
}

static void add(hid_set_t *s, unsigned key)
{
    if (key >= KEY_FIRST && key < KEY_MODIFIER) {
        s->bits[key / 8] |= 1 << (key % 8);
    }
}

bool hid_keys_down(const hid_keys_t *keys, const uint8_t *report, size_t len, hid_set_t *down)
{
    memset(down, 0, sizeof(*down));
    if ((size_t)keys->offset + (keys->bitmap ? 1 : 8) * keys->count > len * 8) {
        return false;
    }
    for (unsigned i = 0; i < keys->count; i++) {
        if (keys->bitmap) {
            unsigned bit = keys->offset + i;

            if (report[bit / 8] & (1 << (bit % 8))) {
                add(down, keys->first + i);
            }
        } else {
            uint8_t key = report[keys->offset / 8 + i];

            if (key == KEY_ROLLOVER) {
                return false;
            }
            add(down, key);
        }
    }
    return true;
}

void hid_key_name(uint8_t key, char buf[HID_NAME_SIZE])
{
    static const char *const names[] = {
        [0x28] = "Enter", [0x29] = "Esc", [0x2A] = "Bksp", [0x2B] = "Tab", [0x2C] = "Space",
        [0x2D] = "-", [0x2E] = "=", [0x2F] = "[", [0x30] = "]", [0x31] = "\\", [0x32] = "#",
        [0x33] = ";", [0x34] = "'", [0x35] = "`", [0x36] = ",", [0x37] = ".", [0x38] = "/",
        [0x39] = "Caps", [0x46] = "PrtSc", [0x47] = "ScrLk", [0x48] = "Pause", [0x49] = "Ins",
        [0x4A] = "Home", [0x4B] = "PgUp", [0x4C] = "Del", [0x4D] = "End", [0x4E] = "PgDn",
        [0x4F] = "Right", [0x50] = "Left", [0x51] = "Down", [0x52] = "Up", [0x53] = "NumLk",
        [0x54] = "/", [0x55] = "*", [0x56] = "-", [0x57] = "+", [0x58] = "Enter", [0x63] = ".",
    };

    if (key >= 0x04 && key <= 0x1D) {
        snprintf(buf, HID_NAME_SIZE, "%c", 'A' + key - 0x04);
    } else if (key >= 0x1E && key <= 0x27) {
        snprintf(buf, HID_NAME_SIZE, "%d", (key - 0x1E + 1) % 10);
    } else if (key >= 0x59 && key <= 0x62) {
        snprintf(buf, HID_NAME_SIZE, "%d", (key - 0x59 + 1) % 10);     /* Keypad 1 to 9, 0 */
    } else if (key >= 0x3A && key <= 0x45) {
        snprintf(buf, HID_NAME_SIZE, "F%d", key - 0x3A + 1);
    } else if (key < sizeof(names) / sizeof(names[0]) && names[key]) {
        snprintf(buf, HID_NAME_SIZE, "%s", names[key]);
    } else {
        snprintf(buf, HID_NAME_SIZE, "0x%02X", key);
    }
}
