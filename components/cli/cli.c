/* Command line on the USB serial port. It changes the entries, but it
 * cannot show a password. Only the buttons can type a password. */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"
#include "display.h"
#include "keypad.h"
#include "rng.h"
#include "ui.h"
#include "usbdev.h"
#include "vault.h"
#include "web.h"
#include "cli.h"

#define LINE_SIZE       160
#define FOREVER         UINT32_MAX
#define KBTEST_DELAY_MS 3000
#define WEB_WAIT_MS     5000

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
        out("The USB host is not ready.\n");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        out("Caps Lock did not turn off. Nothing was typed. Turn Caps Lock off.\n");
        break;
    default:
        out("Error: %s.\n", esp_err_to_name(err));
        break;
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

    if (!report(vault_check_text(arg, VAULT_LABEL_MAX), "label")) {
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
    int i = parse_index(&arg);

    if (i < 0) {
        return;
    }
    if (read_secret(secret, sizeof(secret)) && report(vault_set_secret(i, secret), "password")) {
        out("Changed the password of entry %d.\n", i + 1);
    }
    vault_zero(secret, sizeof(secret));
}

static void cmd_name(char *arg)
{
    int i = parse_index(&arg);

    if (i >= 0 && report(vault_set_label(i, arg), "label")) {
        out("Changed the label of entry %d.\n", i + 1);
    }
}

static void cmd_del(char *arg)
{
    char label[VAULT_LABEL_MAX + 1], question[64];
    int i = parse_index(&arg);

    if (i < 0 || vault_label(i, label, sizeof(label)) != ESP_OK) {
        return;
    }
    snprintf(question, sizeof(question), "Delete entry %d (%s)?", i + 1, label);
    if (confirm(question) && report(vault_delete(i), "")) {
        out("Deleted.\n");
    }
}

static void cmd_move(char *arg)
{
    int from = parse_index(&arg);
    int to = from < 0 ? -1 : parse_index(&arg);

    if (to >= 0 && report(vault_move(from, to), "")) {
        cmd_list(NULL);
    }
}

static void cmd_wipe(char *arg)
{
    char answer[8];

    out("This erases all entries. Type YES to continue: ");
    if (read_line(answer, sizeof(answer), true, FOREVER) < 0 || strcmp(answer, "YES") != 0) {
        out("Nothing changed.\n");
        return;
    }
    if (report(vault_wipe(), "")) {
        out("The vault is empty.\n");
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
    if (!report(usbdev_type(s_kbtest, true), "")) {
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

static void cmd_mouse(char *arg)
{
    bool on = !usbdev_mouse_mover_on();

    ui_mouse(on);
    if (on) {
        out("Mouse mover on: the pointer moves 1 pixel each %d s.\n", USBDEV_MOUSE_PERIOD_S);
    } else {
        out("Mouse mover off.\n");
    }
}

static void cmd_rotate(char *arg)
{
    ui_flip();
    out("The screen turns 180 degrees.\n");
}

static void cmd_info(char *arg)
{
    const esp_app_desc_t *app = esp_app_get_description();
    nvs_stats_t stats = { 0 };
    char name[KEYPAD_NAME_MAX + 1];

    nvs_get_stats("vault", &stats);
    out("Firmware:  %s %s (ESP-IDF %s)\n", app->project_name, app->version, app->idf_ver);
    out("Entries:   %d of %d\n", vault_count(), VAULT_MAX);
    out("NVS:       %u of %u slots used\n", (unsigned)stats.used_entries, (unsigned)stats.total_entries);
    out("USB host:  %s, Caps Lock %s\n", usbdev_ready() ? "ready" : "not ready",
        usbdev_caps_lock() ? "on" : "off");
    out("Mouse:     mover %s\n", usbdev_mouse_mover_on() ? "on" : "off");
    out("Wi-Fi:     %s\n", !web_active() ? "off" : web_connected() ? "on, 1 device" : "on, no device");
    keypad_name(name, sizeof(name));
    switch (keypad_state()) {
    case KEYPAD_NONE:
        out("Keypad:    none\n");
        break;
    case KEYPAD_WAITING:
        out("Keypad:    %s, not connected\n", name);
        break;
    case KEYPAD_CONNECTED:
        out("Keypad:    %s, connected\n", name);
        break;
    }
    out("Heap:      %u bytes free, %u minimum\n", (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size());
    out("LCD:       power mode 0x%02X (0x9C is correct)\n", display_power_mode());
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
    { "wipe", "", "Erase all entries.", cmd_wipe },
    { "kbtest", "", "Type all ASCII characters. Checks the key layout.", cmd_kbtest },
    { "web", "", "Start or stop the Wi-Fi setup mode.", cmd_web },
    { "unpair", "", "Remove the Bluetooth keypad.", cmd_unpair },
    { "mouse", "", "Turn the mouse mover on or off.", cmd_mouse },
    { "rotate", "", "Turn the screen 180 degrees.", cmd_rotate },
    { "info", "", "Show the device status.", cmd_info },
    { "dfu", "", "Restart into download mode for flashing.", cmd_dfu },
};

#define CMD_COUNT (sizeof(s_cmds) / sizeof(s_cmds[0]))

static void cmd_help(char *arg)
{
    char usage[32];

    for (size_t i = 0; i < CMD_COUNT; i++) {
        snprintf(usage, sizeof(usage), "%s %s", s_cmds[i].name, s_cmds[i].args);
        out("  %-20s %s\n", usage, s_cmds[i].help);
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
