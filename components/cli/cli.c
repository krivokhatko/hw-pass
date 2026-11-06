/* Command line on the USB serial port. It changes the entries, but it
 * cannot show a password. Only the buttons can type a password. */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"
#include "battery.h"
#include "blehid.h"
#include "clock.h"
#include "display.h"
#include "hid.h"
#include "imu.h"
#include "keypad.h"
#include "mover.h"
#include "ntp.h"
#include "rng.h"
#include "sound.h"
#include "ui.h"
#include "usbdev.h"
#include "vault.h"
#include "web.h"
#include "cli.h"

#define LINE_SIZE       160
#define FOREVER         UINT32_MAX
#define KBTEST_DELAY_MS 3000
#define WEB_WAIT_MS     5000
#define NTP_WAIT_MS     45000

static const char s_kbtest[] = " !\"#$%&'()*+,-./0123456789:;<=>?@"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                               "abcdefghijklmnopqrstuvwxyz{|}~";

/* ---- Input and output ---- */

static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        usbdev_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    }
}

static void drain_input(void)
{
    char buf[32];

    while (usbdev_read(buf, sizeof(buf), 0) > 0) {
    }
}

/* Read one line. Return its length, -1 after Ctrl+C or when the terminal
 * closes the port, or -2 after timeout_ms without input. Escape
 * sequences (arrow keys) are ignored. */
static int read_line(char *buf, size_t size, bool echo, uint32_t timeout_ms)
{
    static bool skip_lf;
    bool was_open = usbdev_serial_open();
    size_t len = 0;
    uint32_t idle_ms = 0;
    int esc = 0;
    char c;

    for (;;) {
        if (was_open && !usbdev_serial_open()) {
            return -1;
        }
        if (usbdev_read(&c, 1, 100) == 0) {
            idle_ms += 100;
            if (len == 0 && idle_ms >= timeout_ms) {
                return -2;
            }
            continue;
        }
        if (c == '\n' && skip_lf) {
            skip_lf = false;
            continue;
        }
        skip_lf = c == '\r';
        if (esc == 1) {
            esc = (c == '[' || c == 'O') ? 2 : 0;
            continue;
        }
        if (esc == 2) {
            esc = (c >= 0x40 && c <= 0x7E) ? 0 : 2;
            continue;
        }
        switch (c) {
        case '\r':
        case '\n':
            buf[len] = '\0';
            out("\n");
            return len;
        case 0x03:
            out("^C\n");
            return -1;
        case 0x1B:
            esc = 1;
            break;
        case 0x08:
        case 0x7F:
            if (len > 0) {
                len--;
                if (echo) {
                    out("\b \b");
                }
            }
            break;
        default:
            if (c >= 0x20 && c <= 0x7E && len + 1 < size) {
                buf[len++] = c;
                if (echo) {
                    usbdev_write(&c, 1);
                }
            }
            break;
        }
    }
}

static bool confirm(const char *question)
{
    char answer[8];

    out("%s [y/N] ", question);
    if (read_line(answer, sizeof(answer), true, FOREVER) > 0 && tolower((unsigned char)answer[0]) == 'y') {
        return true;
    }
    out("Nothing changed.\n");
    return false;
}

/* Ask for the password two times without echo. */
static bool read_secret(char *buf, size_t size)
{
    char again[LINE_SIZE];
    bool same;

    out("Password: ");
    if (read_line(buf, size, false, FOREVER) < 0) {
        return false;
    }
    out("Again: ");
    if (read_line(again, sizeof(again), false, FOREVER) < 0) {
        vault_zero(buf, size);
        return false;
    }
    same = strcmp(buf, again) == 0;
    vault_zero(again, sizeof(again));
    if (!same) {
        out("The two passwords are not the same. Nothing changed.\n");
        vault_zero(buf, size);
    }
    return same;
}

static bool report(esp_err_t err, const char *what)
{
    switch (err) {
    case ESP_OK:
        return true;
    case ESP_ERR_NOT_FOUND:
        out("No entry has this number. Use \"list\".\n");
        break;
    case ESP_ERR_NO_MEM:
        out("The vault is full (%d entries).\n", VAULT_MAX);
        break;
    case ESP_ERR_INVALID_SIZE:
        out("The %s must have 1 to %d characters.\n", what,
            strcmp(what, "label") == 0 ? VAULT_LABEL_MAX : VAULT_SECRET_MAX);
        break;
    case ESP_ERR_INVALID_ARG:
        out("The %s must contain only printable ASCII characters.\n", what);
        break;
    case ESP_ERR_INVALID_STATE:
        out("No computer is ready: no USB host, and no Bluetooth computer.\n");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        out("Caps Lock did not turn off. Nothing was typed. Turn Caps Lock off.\n");
        break;
    case ESP_ERR_NOT_ALLOWED:
        out("The device is locked. Enter the PIN on the device, or use \"unlock\".\n");
        break;
    default:
        out("Error: %s.\n", esp_err_to_name(err));
        break;
    }
    return false;
}

/* Print a message and return true if the entries are not available. */
static bool locked(void)
{
    if (vault_locked()) {
        report(ESP_ERR_NOT_ALLOWED, "");
        return true;
    }
    return false;
}

/* Parse a 1-based entry number. Return the 0-based index or -1. */
static int parse_index(char **arg)
{
    char *end;
    long n = strtol(*arg, &end, 10);

    if (end == *arg || (*end != ' ' && *end != '\0') || n < 1 || n > vault_count()) {
        out("Give an entry number from 1 to %d.\n", vault_count());
        return -1;
    }
    while (*end == ' ') {
        end++;
    }
    *arg = end;
    return n - 1;
}

/* ---- Commands ---- */

static void cmd_help(char *arg);

static void cmd_list(char *arg)
{
    char label[VAULT_LABEL_MAX + 1], key[KEYPAD_KEY_NAME];

    if (locked()) {
        return;
    }
    if (vault_count() == 0) {
        out("The vault is empty. Use \"add <label>\".\n");
    }
    for (int i = 0; i < vault_count(); i++) {
        if (vault_label(i, label, sizeof(label)) != ESP_OK) {
            continue;
        }
        if (vault_key(i) != 0) {
            keypad_key_name(vault_key(i), key);
            out("%3d  %-*s  key %s\n", i + 1, VAULT_LABEL_MAX, label, key);
        } else {
            out("%3d  %s\n", i + 1, label);
        }
    }
}

static void cmd_add(char *arg)
{
    char secret[LINE_SIZE];

    if (locked() || !report(vault_check_text(arg, VAULT_LABEL_MAX), "label")) {
        return;
    }
    if (vault_count() >= VAULT_MAX) {
        report(ESP_ERR_NO_MEM, "");
        return;
    }
    if (read_secret(secret, sizeof(secret)) && report(vault_add(arg, secret), "password")) {
        out("Added entry %d: %s\n", vault_count(), arg);
    }
    vault_zero(secret, sizeof(secret));
}

static void cmd_gen(char *arg)
{
    char secret[VAULT_SECRET_MAX + 1];
    char *label;
    long len = strtol(arg, &label, 10);

    if (locked()) {
        return;
    }
    if (label == arg || (*label != ' ' && *label != '\0')) {
        len = RNG_PASSWORD_DEFAULT;     /* No length. The label can start with a digit. */
        label = arg;
    }
    while (*label == ' ') {
        label++;
    }
    if (len < RNG_PASSWORD_MIN || len > VAULT_SECRET_MAX) {
        out("The length must be %d to %d.\n", RNG_PASSWORD_MIN, VAULT_SECRET_MAX);
        return;
    }
    if (!report(vault_check_text(label, VAULT_LABEL_MAX), "label")) {
        return;
    }
    rng_password(secret, len);
    if (report(vault_add(label, secret), "password")) {
        out("Added entry %d: %s, with a random password of %ld characters.\n", vault_count(), label, len);
    }
    vault_zero(secret, sizeof(secret));
}

static void cmd_pass(char *arg)
{
    char secret[LINE_SIZE];
    int i;

    if (locked() || (i = parse_index(&arg)) < 0) {
        return;
    }
    if (read_secret(secret, sizeof(secret)) && report(vault_set_secret(i, secret), "password")) {
        out("Changed the password of entry %d.\n", i + 1);
    }
    vault_zero(secret, sizeof(secret));
}

static void cmd_name(char *arg)
{
    int i;

    if (locked()) {
        return;
    }
    i = parse_index(&arg);
    if (i >= 0 && report(vault_set_label(i, arg), "label")) {
        out("Changed the label of entry %d.\n", i + 1);
    }
}

static void cmd_del(char *arg)
{
    char label[VAULT_LABEL_MAX + 1], question[64];
    int i;

    if (locked() || (i = parse_index(&arg)) < 0 || vault_label(i, label, sizeof(label)) != ESP_OK) {
        return;
    }
    snprintf(question, sizeof(question), "Delete entry %d (%s)?", i + 1, label);
    if (confirm(question) && report(vault_delete(i), "")) {
        out("Deleted.\n");
    }
}

static void cmd_move(char *arg)
{
    int from, to;

    if (locked()) {
        return;
    }
    from = parse_index(&arg);
    to = from < 0 ? -1 : parse_index(&arg);
    if (to >= 0 && report(vault_move(from, to), "")) {
        cmd_list(NULL);
    }
}

static void cmd_wipe(char *arg)
{
    char answer[8];

    out("This erases all entries%s. Type YES to continue: ", vault_has_pin() ? " and the PIN" : "");
    if (read_line(answer, sizeof(answer), true, FOREVER) < 0 || strcmp(answer, "YES") != 0) {
        out("Nothing changed.\n");
        return;
    }
    if (report(vault_wipe(), "")) {
        out("The vault is empty.\n");
    }
}

/* Ask for the PIN without echo. Return false after Ctrl+C. */
static bool read_pin(const char *prompt, char *buf, size_t size)
{
    out("%s", prompt);
    return read_line(buf, size, false, FOREVER) >= 0;
}

static void pin_hint(void)
{
    if (!vault_efuse_key()) {
        out("The chip has no HMAC key in eFuse: a copy of the flash can be attacked off the device.\n"
            "See \"PIN\" in docs/usage.md for the eFuse key.\n");
    }
}

/* "pin" sets or changes the PIN. "pin off" removes it. */
static void cmd_pin(char *arg)
{
    char pin[LINE_SIZE], again[LINE_SIZE];
    esp_err_t err;

    if (strcmp(arg, "off") == 0) {
        if (!vault_has_pin()) {
            out("No PIN is set.\n");
        } else if (!locked() && confirm("Remove the PIN? The entries become plain text in flash.")) {
            if (report(vault_clear_pin(), "")) {
                out("The PIN is removed.\n");
            }
        }
        return;
    }
    if (*arg != '\0') {
        out("Use \"pin\" to set or change the PIN, or \"pin off\" to remove it.\n");
        return;
    }
    if (locked()) {
        return;
    }
    if (!read_pin(vault_has_pin() ? "New PIN (4 to 12 digits): " : "PIN (4 to 12 digits): ", pin, sizeof(pin))) {
        return;
    }
    err = vault_check_pin(pin);
    if (err != ESP_OK) {
        out("The PIN must have %d to %d digits. Nothing changed.\n", VAULT_PIN_MIN, VAULT_PIN_MAX);
    } else if (read_pin("Again: ", again, sizeof(again))) {
        if (strcmp(pin, again) != 0) {
            out("The two PINs are not the same. Nothing changed.\n");
        } else if (report(vault_set_pin(pin), "")) {
            out("The PIN is set. The entries are encrypted with a key from the PIN%s.\n",
                vault_pin_bound() ? " and the eFuse key" : "");
            out("The device asks for the PIN after a start and after a stop of the USB connection.\n");
            pin_hint();
        }
    }
    vault_zero(pin, sizeof(pin));
    vault_zero(again, sizeof(again));
}

static void cmd_unlock(char *arg)
{
    char pin[LINE_SIZE];
    esp_err_t err;

    if (!vault_has_pin()) {
        out("No PIN is set. Use \"pin\".\n");
        return;
    }
    if (!vault_locked()) {
        out("The device is not locked.\n");
        return;
    }
    if (vault_wait_s() > 0) {
        out("Wait %d s before the next try.\n", vault_wait_s());
        return;
    }
    if (!read_pin("PIN: ", pin, sizeof(pin))) {
        return;
    }
    err = vault_unlock(pin);
    vault_zero(pin, sizeof(pin));
    if (err == ESP_OK) {
        out("Unlocked. %d entries.\n", vault_count());
    } else if (err == ESP_ERR_INVALID_ARG) {
        out("Wrong PIN (%d in a row).", vault_fails());
        if (vault_wait_s() > 0) {
            out(" Wait %d s before the next try.", vault_wait_s());
        }
        out("\n");
    } else {
        report(err, "");
    }
}

static void cmd_lock(char *arg)
{
    if (!vault_has_pin()) {
        out("No PIN is set. Use \"pin\".\n");
        return;
    }
    vault_lock();
    out("Locked.\n");
}

/* "layout" shows the layout key. "layout F16" or "layout off" sets it. */
static void cmd_layout(char *arg)
{
    char text[HID_LAYOUT_TEXT];
    size_t len = strlen(arg);

    while (len > 0 && arg[len - 1] == ' ') {
        arg[--len] = '\0';
    }
    if (len > 0 && hid_set_layout(arg) != ESP_OK) {
        out("Give a key, with modifiers if needed, or \"off\": for example \"layout F16\" or\n"
            "\"layout ctrl+shift+1\". Modifiers: ctrl, shift, alt, win. Keys: F1 to F24, 0 to 9, a to z, space.\n");
        return;
    }
    hid_layout_text(text, sizeof(text));
    if (strcmp(text, "off") == 0) {
        out("Layout key: off. The device types without a layout change.\n");
    } else {
        out("Layout key: %s. The device pushes it before it types.\n", text);
    }
}

static void cmd_kbtest(char *arg)
{
    char line[LINE_SIZE];
    int n;

    out("Put the focus on this terminal or a text editor. Typing starts in %d s.\n",
        KBTEST_DELAY_MS / 1000);
    vTaskDelay(pdMS_TO_TICKS(KBTEST_DELAY_MS));
    drain_input();
    if (!report(hid_type(s_kbtest, true), "")) {
        return;
    }
    n = read_line(line, sizeof(line), false, KBTEST_DELAY_MS);
    if (n < 0) {
        out("No text came back. Compare the typed text with:\n%s\n", s_kbtest);
    } else if (strcmp(line, s_kbtest) == 0) {
        out("OK. The host key layout is US.\n");
    } else {
        out("The text is different. The host key layout is not US.\nSent: %s\nGot:  %s\n", s_kbtest, line);
    }
}

static void cmd_unpair(char *arg)
{
    char name[KEYPAD_NAME_MAX + 1], question[64];
    esp_err_t err;

    keypad_name(name, sizeof(name));
    if (keypad_state() == KEYPAD_NONE) {
        out("No keypad is paired.\n");
        return;
    }
    snprintf(question, sizeof(question), "Remove the keypad %s?", name);
    if (!confirm(question)) {
        return;
    }
    err = keypad_unpair();
    if (err == ESP_OK) {
        out("The keypad is removed. The entries keep their keys.\n");
    } else {
        out("Error: %s.\n", esp_err_to_name(err));
    }
}

/* "wireless" shows the Bluetooth computer. "wireless pair" starts a
 * pairing: the screen shows the passkey. "wireless forget" removes the
 * computer. */
static void cmd_wireless(char *arg)
{
    char name[BLEHID_NAME_MAX + 1], question[64];
    blehid_state_t state = blehid_state();
    size_t len = strlen(arg);
    esp_err_t err;

    while (len > 0 && arg[len - 1] == ' ') {
        arg[--len] = '\0';
    }
    blehid_name(name, sizeof(name));
    if (strcmp(arg, "pair") == 0) {
        if (locked()) {
            return;
        }
        ui_pair();
        out("Look at the screen. Add \"hw-pass\" in the Bluetooth settings of the computer,\n"
            "then type the passkey from the screen. A button on the device stops the pairing.\n");
    } else if (strcmp(arg, "forget") == 0) {
        if (state == BLEHID_NONE) {
            out("No computer is paired.\n");
            return;
        }
        snprintf(question, sizeof(question), "Remove the computer %s?", name);
        if (!confirm(question)) {
            return;
        }
        err = blehid_unpair();
        if (err == ESP_OK) {
            out("The computer is removed. Also remove \"hw-pass\" on the computer.\n");
        } else {
            out("Error: %s.\n", esp_err_to_name(err));
        }
    } else if (len > 0) {
        out("Use \"wireless\", \"wireless pair\", or \"wireless forget\".\n");
    } else if (state == BLEHID_NONE) {
        out("No computer is paired. Use \"wireless pair\", or hold the two buttons for 8 s.\n");
    } else if (state == BLEHID_WAITING) {
        out("Computer %s: not connected. The device waits for it.\n", name);
    } else {
        out("Computer %s: connected%s.\n", name, hid_link() == HID_LINK_BLE ? ", it gets the keys" : "");
    }
}

static void cmd_mouse(char *arg)
{
    char next[MOVER_NEXT_TEXT];
    bool on = !mover_on();

    mover_set(on);
    if (on) {
        out("Mouse mover on: the pointer moves 1 pixel each %d s.", HID_MOUSE_PERIOD_S);
    } else {
        out("Mouse mover off.");
    }
    mover_next_text(next, sizeof(next));
    if (next[0]) {
        out(" Next: %s.", next);
    }
    out("\n");
}

/* "on at 09:00 Mon-Fri, off at 21:00 Mon-Thu". Without days: "never on". */
static void sched_times(const mover_sched_t *s, char *buf, size_t size)
{
    char days[MOVER_DAYS_TEXT];
    size_t n;

    if (s->on_days == 0) {
        n = snprintf(buf, size, "never on");
    } else {
        mover_days_text(s->on_days, days, sizeof(days));
        n = snprintf(buf, size, "on at %02d:%02d %s", s->on_min / 60, s->on_min % 60, days);
    }
    if (s->off_days == 0) {
        snprintf(buf + n, size - n, ", never off");
    } else {
        mover_days_text(s->off_days, days, sizeof(days));
        snprintf(buf + n, size - n, ", off at %02d:%02d %s", s->off_min / 60, s->off_min % 60, days);
    }
}

static void show_sched(void)
{
    mover_sched_t s;
    char times[80], next[MOVER_NEXT_TEXT];

    mover_get_sched(&s);
    sched_times(&s, times, sizeof(times));
    out("The schedule is %s. Times: %s.", s.enabled ? "on" : "off", times);
    if (!s.enabled) {
        out("\n");
    } else if (!clock_valid()) {
        out(" The time is not set: the schedule waits. Use \"time\".\n");
    } else {
        mover_next_text(next, sizeof(next));
        if (next[0]) {
            out(" Next: %s.\n", next);
        } else {
            out(" No change comes.\n");
        }
    }
}

static void auto_usage(void)
{
    out("Use \"auto\" to turn the schedule on or off.\n"
        "Use \"auto on|off [HH:MM] [days]\" to set the time or the days of the auto-on or the auto-off,\n"
        "for example \"auto off 21:00 mon-thu\". Days: all, none, mon,wed, mon-fri, fri-mon.\n");
}

/* "auto" turns the schedule on or off. "auto on 09:00 mon-fri" and
 * "auto off 21:00 mon-thu" set a time, days, or both. */
static void cmd_auto(char *arg)
{
    mover_sched_t s;
    char *tok, *save;
    uint16_t *min;
    uint8_t *days, acc = 0;
    bool got_time = false, got_days = false;

    mover_get_sched(&s);
    if (*arg == '\0') {
        s.enabled = !s.enabled;
    } else {
        tok = strtok_r(arg, " ", &save);
        if (strcmp(tok, "on") == 0) {
            min = &s.on_min;
            days = &s.on_days;
        } else if (strcmp(tok, "off") == 0) {
            min = &s.off_min;
            days = &s.off_days;
        } else {
            auto_usage();
            return;
        }
        while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
            int h, m, n = 0;
            uint8_t d;

            if (sscanf(tok, "%2d:%2d%n", &h, &m, &n) == 2 && tok[n] == '\0' &&
                h >= 0 && h < 24 && m >= 0 && m < 60) {
                *min = h * 60 + m;
                got_time = true;
            } else if (mover_parse_days(tok, &d)) {
                acc |= d;
                got_days = true;
            } else {
                out("\"%s\" is not a time (HH:MM) or days.\n", tok);
                auto_usage();
                return;
            }
        }
        if (!got_time && !got_days) {
            auto_usage();
            return;
        }
        if (got_days) {
            *days = acc;
        }
    }
    if (mover_set_sched(&s) != ESP_OK) {
        out("The schedule is not valid. Nothing changed.\n");
        return;
    }
    show_sched();
}

/* "2026-10-31 14:30:02 Sat EET", or "" if the time is not set. */
static void time_text(char *buf, size_t size)
{
    struct tm t;

    if (clock_get(&t)) {
        strftime(buf, size, "%Y-%m-%d %H:%M:%S %a %Z", &t);
    } else {
        buf[0] = '\0';
    }
}

static void show_time(void)
{
    char text[40];

    time_text(text, sizeof(text));
    if (text[0]) {
        out("%s\n", text);
    } else {
        out("The time is not set. Use \"time YYYY-MM-DD HH:MM\", or \"ntp\" with a network.\n");
    }
}

static void cmd_time(char *arg)
{
    struct tm t;

    if (*arg != '\0' && (clock_parse(arg, &t) != ESP_OK || clock_set(&t) != ESP_OK)) {
        out("Give the date and the time as YYYY-MM-DD HH:MM, for example \"time 2026-10-31 14:30\".\n");
        return;
    }
    show_time();
}

static void cmd_tz(char *arg)
{
    char tz[CLOCK_TZ_MAX + 1];
    size_t len = strlen(arg);

    while (len > 0 && arg[len - 1] == ' ') {
        arg[--len] = '\0';
    }
    if (len > 0 && clock_set_tz(arg) != ESP_OK) {
        out("Give a POSIX time zone, for example \"tz EET-2EEST,M3.5.0/3,M10.5.0/4\" or \"tz UTC0\".\n"
            "See \"Time zone\" in docs/usage.md.\n");
        return;
    }
    clock_get_tz(tz, sizeof(tz));
    out("Time zone: %s\n", tz);
    show_time();
}

/* Print the error of a time sync function. Return true if there is none. */
static bool ntp_report(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return true;
    case ESP_ERR_NOT_SUPPORTED:
        out("The time sync is not in this build (CONFIG_HWPASS_NTP).\n");
        break;
    case ESP_ERR_NOT_FOUND:
        out("No network. Use \"wifi <name>\" first.\n");
        break;
    case ESP_ERR_INVALID_STATE:
        out("The Wi-Fi setup mode is on. Stop it first.\n");
        break;
    default:
        out("Error: %s.\n", esp_err_to_name(err));
        break;
    }
    return false;
}

/* The Wi-Fi password of the time sync is not a vault secret, but it
 * does not echo, and the device never shows it. */
static void cmd_wifi(char *arg)
{
    char ssid[NTP_SSID_MAX + 1], pass[LINE_SIZE];
    size_t len = strlen(arg);
    esp_err_t err;

    while (len > 0 && arg[len - 1] == ' ') {
        arg[--len] = '\0';
    }
    ntp_get_ssid(ssid, sizeof(ssid));
    if (len == 0) {
        if (ssid[0]) {
            out("Network of the time sync: \"%s\". Use \"wifi <name>\" to change it, or \"wifi off\" to remove it.\n", ssid);
        } else {
            out("No network for the time sync. Use \"wifi <name>\".\n");
        }
        return;
    }
    if (strcmp(arg, "off") == 0) {
        ntp_clear_wifi();
        out(ssid[0] ? "The network is removed. The device does not sync the time.\n" : "No network is set.\n");
        return;
    }
    if (len > NTP_SSID_MAX) {
        out("The network name must have 1 to %d characters.\n", NTP_SSID_MAX);
        return;
    }
    out("Wi-Fi password: ");
    if (read_line(pass, sizeof(pass), false, FOREVER) < 0) {
        vault_zero(pass, sizeof(pass));
        return;
    }
    err = ntp_set_wifi(arg, pass);
    vault_zero(pass, sizeof(pass));
    if (err == ESP_ERR_INVALID_SIZE) {
        out("The password must have %d to %d characters: the network must use WPA2 or WPA3. Nothing changed.\n",
            NTP_PASS_MIN, NTP_PASS_MAX);
    } else if (err == ESP_ERR_INVALID_ARG) {
        out("The name and the password must contain only printable ASCII characters. Nothing changed.\n");
    } else if (ntp_report(err)) {
        out("The network \"%s\" is saved. Type \"ntp\" to sync the time now.\n", arg);
    }
}

static bool valid_host(const char *s)
{
    if (*s == '\0' || strlen(s) > NTP_SERVER_MAX) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') {
            return false;
        }
    }
    return true;
}

static void ntp_usage(void)
{
    out("Use \"ntp\" to sync the time now, \"ntp on\" or \"ntp off\" for the automatic sync,\n"
        "or \"ntp server <name or IP>\" to set the NTP server (\"ntp server router\": the router).\n");
}

static void cmd_ntp(char *arg)
{
    char text[NTP_RESULT_TEXT], *tok, *save;
    uint32_t count = ntp_count();

    if (*arg == '\0') {
        if (!ntp_report(ntp_request())) {
            return;
        }
        out("The time sync runs. It takes some seconds.\n");
        for (int i = 0; i < NTP_WAIT_MS / 100 && ntp_count() == count; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ntp_count() == count) {
            out("The sync did not end. Look at \"info\" later.\n");
            return;
        }
        ntp_result_text(text, sizeof(text));
        out("Sync %s.\n", text);
        if (strncmp(text, "OK", 2) == 0) {
            show_time();
        }
        return;
    }
    tok = strtok_r(arg, " ", &save);
    if (strcmp(tok, "on") == 0 || strcmp(tok, "off") == 0) {
        bool on = strcmp(tok, "on") == 0;

        if (ntp_report(ntp_set_auto(on))) {
            out("Automatic time sync %s.\n", on ? "on: after each start, then each 24 h" : "off");
        }
    } else if (strcmp(tok, "server") == 0) {
        tok = strtok_r(NULL, " ", &save);
        if (tok == NULL || strtok_r(NULL, " ", &save) != NULL) {
            ntp_usage();
            return;
        }
        if (strcmp(tok, "router") == 0) {
            tok = "";
        } else if (!valid_host(tok)) {
            out("Give a host name or an IP address, for example \"ntp server 192.168.1.1\".\n");
            return;
        }
        if (ntp_report(ntp_set_server(tok))) {
            out("NTP server: %s.\n", tok[0] ? tok : "the router");
        }
    } else {
        ntp_usage();
    }
}

static void cmd_sound(char *arg)
{
    bool on = !sound_enabled();

    ui_sound(on);
    out("Sounds %s.\n", on ? "on" : "off");
    if (on && !sound_ready()) {
        out("The speaker does not work: the audio codec did not start.\n");
    }
}

static void cmd_rotate(char *arg)
{
    ui_flip();
    out("The screen turns 180 degrees. It holds until you turn the device.\n");
}

static void cmd_info(char *arg)
{
    const esp_app_desc_t *app = esp_app_get_description();
    nvs_stats_t stats = { 0 };
    mover_sched_t sched;
    char name[KEYPAD_NAME_MAX + 1], battery[24] = "", next[MOVER_NEXT_TEXT], times[80], text[40];
    char tz[CLOCK_TZ_MAX + 1], ssid[NTP_SSID_MAX + 1], server[NTP_SERVER_MAX + 1], result[NTP_RESULT_TEXT];
    int level = keypad_battery();
    float g[3];

    nvs_get_stats("vault", &stats);
    out("Firmware:  %s %s (ESP-IDF %s)\n", app->project_name, app->version, app->idf_ver);
    if (vault_locked()) {
        out("Entries:   locked\n");
    } else {
        out("Entries:   %d of %d\n", vault_count(), VAULT_MAX);
    }
    if (!vault_has_pin()) {
        out("PIN:       none. Use \"pin\".\n");
    } else {
        out("PIN:       set, key %s the eFuse key, %s", vault_pin_bound() ? "with" : "without",
            vault_locked() ? "locked" : "unlocked");
        if (vault_fails() > 0) {
            out(", %d wrong in a row", vault_fails());
        }
        if (vault_wait_s() > 0) {
            out(", wait %d s", vault_wait_s());
        }
        out("\n");
    }
    out("eFuse:     %s\n", vault_efuse_key() ? "HMAC key present" : "no HMAC key");
    out("NVS:       %u of %u slots used\n", (unsigned)stats.used_entries, (unsigned)stats.total_entries);
    out("USB host:  %s, Caps Lock %s\n", usbdev_ready() ? "ready" : "not ready",
        usbdev_caps_lock() ? "on" : "off");
    if (battery_level() >= 0) {
        out("Battery:   %d %%, %.2f V%s%s\n", battery_level(), battery_mv() / 1000.0,
            battery_usb() ? ", USB power" : "", battery_charging() ? ", charging" : "");
    } else {
        out("Battery:   no measurement%s\n", battery_usb() ? ", USB power" : "");
    }
    blehid_name(name, sizeof(name));
    switch (blehid_state()) {
    case BLEHID_NONE:
        out("Computer:  none. Use \"wireless pair\".\n");
        break;
    case BLEHID_WAITING:
        out("Computer:  %s, not connected\n", name);
        break;
    case BLEHID_CONNECTED:
        out("Computer:  %s, connected, Caps Lock %s\n", name, blehid_caps_lock() ? "on" : "off");
        break;
    }
    out("Keys go:   %s\n", hid_link() == HID_LINK_BLE ? "to the computer over Bluetooth" :
        hid_link() == HID_LINK_USB ? "to the USB host" : "nowhere: no computer is ready");
    hid_layout_text(text, sizeof(text));
    out("Layout:    key %s\n", text);
    mover_next_text(next, sizeof(next));
    out("Mouse:     mover %s%s%s\n", mover_on() ? "on" : "off", next[0] ? ", next: " : "", next);
    mover_get_sched(&sched);
    sched_times(&sched, times, sizeof(times));
    if (!sched.enabled) {
        out("Schedule:  off (%s)\n", times);
    } else {
        out("Schedule:  %s%s\n", times, clock_valid() ? "" : "; waits for the time");
    }
    time_text(text, sizeof(text));
    if (text[0]) {
        out("Time:      %s\n", text);
    } else {
        out("Time:      not set. Use \"time YYYY-MM-DD HH:MM\", or \"ntp\" with a network.\n");
    }
    clock_get_tz(tz, sizeof(tz));
    out("Zone:      %s\n", tz);
    ntp_get_ssid(ssid, sizeof(ssid));
    if (ssid[0]) {
        ntp_get_server(server, sizeof(server));
        out("Sync:      network \"%s\", server %s, auto %s\n", ssid, server[0] ? server : "router",
            ntp_auto() ? "on" : "off");
    } else {
        out("Sync:      no network. Use \"wifi <name>\".\n");
    }
    ntp_result_text(result, sizeof(result));
    if (ntp_busy()) {
        out("Last sync: runs now\n");
    } else if (result[0]) {
        out("Last sync: %s\n", result);
    }
    out("Sound:     %s%s\n", sound_enabled() ? "on" : "off", sound_ready() ? "" : ", audio codec error");
    out("Wi-Fi:     %s\n", ntp_busy() ? "on, time sync" : !web_active() ? "off" :
        web_connected() ? "on, 1 device" : "on, no device");
    keypad_name(name, sizeof(name));
    if (level >= 0) {
        snprintf(battery, sizeof(battery), ", battery %d%%", level);
    }
    switch (keypad_state()) {
    case KEYPAD_NONE:
        out("Keypad:    none\n");
        break;
    case KEYPAD_WAITING:
        out("Keypad:    %s, not connected%s\n", name, battery);
        break;
    case KEYPAD_CONNECTED:
        out("Keypad:    %s, connected%s\n", name, battery);
        break;
    }
    out("Heap:      %u bytes free, %u minimum\n", (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size());
    out("LCD:       power mode 0x%02X (0x9C is correct)\n", display_power_mode());
    if (imu_read(g) == ESP_OK) {
        imu_up_t up = imu_up();

        out("Tilt:      %s, x %+.2f y %+.2f z %+.2f g\n",
            up == IMU_TOP_UP ? "top edge up" : up == IMU_BOTTOM_UP ? "bottom edge up" : "flat",
            g[0], g[1], g[2]);
    } else {
        out("Tilt:      no sensor\n");
    }
}

/* The Wi-Fi password is not a vault secret. It is new at each start. */
static void cmd_web(char *arg)
{
    web_info_t info;

    if (web_active()) {
        ui_web(false);
        for (int i = 0; i < WEB_WAIT_MS / 100 && web_active(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        out("Wi-Fi setup mode is %s.\n", web_active() ? "still on" : "off");
        return;
    }
    if (locked()) {
        return;
    }
    ui_web(true);
    for (int i = 0; i < WEB_WAIT_MS / 100 && web_get_info(&info) != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (web_get_info(&info) != ESP_OK) {
        out("Wi-Fi setup mode did not start. Look at the screen.\n");
        return;
    }
    out("Wi-Fi setup mode is on. Push a button on the device to stop it.\n");
    out("Network:   %s\nPassword:  %s\nPage:      http://%s/\n", info.ssid, info.password, WEB_IP);
    vault_zero(&info, sizeof(info));
}

static void cmd_dfu(char *arg)
{
    out("Restart into download mode. Then flash with \"idf.py -p <port> flash\".\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    ui_dfu();
}

static const struct {
    const char *name;
    const char *args;
    const char *help;
    void (*fn)(char *arg);
} s_cmds[] = {
    { "help", "", "Show the commands.", cmd_help },
    { "list", "", "Show the entries.", cmd_list },
    { "add", "<label>", "Add an entry. Asks for the password.", cmd_add },
    { "gen", "[len] <label>", "Add an entry with a random password (20).", cmd_gen },
    { "pass", "<n>", "Change the password of entry n.", cmd_pass },
    { "name", "<n> <label>", "Change the label of entry n.", cmd_name },
    { "del", "<n>", "Delete entry n.", cmd_del },
    { "move", "<n> <pos>", "Move entry n to position pos.", cmd_move },
    { "wipe", "", "Erase all entries and the PIN.", cmd_wipe },
    { "pin", "[off]", "Set or change the PIN, or remove it.", cmd_pin },
    { "unlock", "", "Enter the PIN.", cmd_unlock },
    { "lock", "", "Lock the device now.", cmd_lock },
    { "layout", "[key|off]", "Key that selects the host key layout before typing.", cmd_layout },
    { "kbtest", "", "Type all ASCII characters. Checks the key layout.", cmd_kbtest },
    { "web", "", "Start or stop the Wi-Fi setup mode.", cmd_web },
    { "unpair", "", "Remove the Bluetooth keypad.", cmd_unpair },
    { "wireless", "[pair|forget]", "Bluetooth computer: show, pair, or remove it.", cmd_wireless },
    { "mouse", "", "Turn the mouse mover on or off.", cmd_mouse },
    { "auto", "[on|off HH:MM days]", "Schedule of the mouse mover: toggle, or set.", cmd_auto },
    { "time", "[YYYY-MM-DD HH:MM]", "Show or set the time.", cmd_time },
    { "tz", "[zone]", "Show or set the time zone (POSIX TZ).", cmd_tz },
    { "wifi", "[<name>|off]", "Network of the time sync. Asks for the password.", cmd_wifi },
    { "ntp", "[on|off|server x]", "Sync the time now, or set the automatic sync.", cmd_ntp },
    { "sound", "", "Turn the sounds on or off.", cmd_sound },
    { "rotate", "", "Turn the screen 180 degrees, for a device that lies flat.", cmd_rotate },
    { "info", "", "Show the device status.", cmd_info },
    { "dfu", "", "Restart into download mode for flashing.", cmd_dfu },
};

#define CMD_COUNT (sizeof(s_cmds) / sizeof(s_cmds[0]))

static void cmd_help(char *arg)
{
    char usage[32];

    for (size_t i = 0; i < CMD_COUNT; i++) {
        snprintf(usage, sizeof(usage), "%s %s", s_cmds[i].name, s_cmds[i].args);
        out("  %-24s %s\n", usage, s_cmds[i].help);
    }
    out("The device never shows a password. Push the front button to type it.\n");
}

static void execute(char *line)
{
    char *arg;

    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }
    arg = line + strcspn(line, " ");
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    }
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strcmp(line, s_cmds[i].name) == 0) {
            s_cmds[i].fn(arg);
            return;
        }
    }
    out("Unknown command \"%s\". Use \"help\".\n", line);
}

/* Some terminals (idf.py monitor) do not set DTR. The command line works
 * without DTR. The banner shows when a terminal sets DTR. */
static void cli_task(void *arg)
{
    char line[LINE_SIZE];
    bool open = false;
    int n;

    for (;;) {
        if (usbdev_serial_open() && !open) {
            out("\nhw-pass %s. Type \"help\" for the commands.\n> ", esp_app_get_description()->version);
        }
        open = usbdev_serial_open();
        n = read_line(line, sizeof(line), true, 200);
        if (n == -2) {
            continue;
        }
        if (n >= 0) {
            execute(line);
        }
        out("> ");
    }
}

esp_err_t cli_start(void)
{
    BaseType_t ok = xTaskCreate(cli_task, "cli", 6144, NULL, 3, NULL);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
