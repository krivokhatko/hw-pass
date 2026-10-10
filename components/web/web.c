/* Wi-Fi setup mode: an access point with a web page that changes the
 * entries. Like the command line, the page cannot show a password.
 *
 * Protection:
 * - The access point has a new random password at each start. It
 *   accepts one device.
 * - Requests must use WEB_IP as the host. This stops DNS rebinding.
 *   Requests for other hosts get a redirect to the page (captive portal).
 * - A change needs the session token in the X-Token header. A page from
 *   another site cannot set this header, because there is no CORS.
 * - A backup contains all passwords. The page can make one only if the
 *   buttons started the mode: then only the screen shows the Wi-Fi
 *   password. The "web" command sends it to the USB host. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "web.h"

#if CONFIG_HWPASS_WEB

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "backup.h"
#include "rng.h"
#include "vault.h"
#include "dns.h"

#define AP_CHANNEL      6
#define AP_TX_POWER     44      /* 0.25 dBm units: 11 dBm. Enough for some meters. */
#define TIMEOUT_MS      (5 * 60 * 1000)
#define TOKEN_LEN       16
#define BODY_MAX        16384   /* An import of all entries, URL-encoded: 14.2 KB */
#define FIELD_MAX       160     /* Longer than a label or a password */
#define RECV_TRIES      3

#define ST_OK           "200 OK"
#define ST_BAD          "400 Bad Request"
#define ST_FORBIDDEN    "403 Forbidden"
#define ST_CONFLICT     "409 Conflict"
#define ST_TOO_LARGE    "413 Content Too Large"
#define ST_ERROR        "500 Internal Server Error"

static const char *TAG = "web";
static const char s_lower[] = "23456789abcdefghjkmnpqrstuvwxyz";   /* No 0, 1, i, l, o */
static const char s_upper[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
static const char s_portal[] = "http://" WEB_IP "/";
static const char *const s_quiet[] = { "wifi", "wifi_init", "phy_init", "pp", "net80211", "esp_netif_lwip" };
static const char s_csp[] = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                            "img-src data:; connect-src 'self'; form-action 'none'; frame-ancestors 'none'";

extern const char s_page_start[] asm("_binary_index_html_start");
extern const char s_page_end[] asm("_binary_index_html_end");

static volatile bool s_active;
static volatile bool s_done;
static volatile bool s_connected;
static bool s_backup_ok;                /* The buttons started the mode. */
static volatile uint32_t s_last_ms;     /* Time of the last request from the page */
static char s_token[TOKEN_LEN + 1];
static web_info_t s_info;
static esp_netif_t *s_netif;
static httpd_handle_t s_server;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool web_active(void)
{
    return s_active;
}

esp_err_t web_get_info(web_info_t *info)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    __sync_synchronize();   /* Pairs with the barrier in web_start. */
    *info = s_info;
    return ESP_OK;
}

bool web_connected(void)
{
    return s_active && s_connected;
}

uint32_t web_left_s(void)
{
    /* Read the request time before the clock. In the other order, a
     * request between the two reads makes the difference negative. */
    uint32_t last = s_last_ms;
    uint32_t idle = now_ms() - last;

    if (!s_active || s_done || idle >= TIMEOUT_MS) {
        return 0;
    }
    return (TIMEOUT_MS - idle + 999) / 1000;
}

/* ---- Responses ---- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* The message must not contain quotes or backslashes. */
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *fmt, ...)
{
    char msg[128], json[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", msg);
    return send_json(req, status, json);
}

/* The list and the limits. Labels only: the page never gets a password.
 * extra: more JSON members, or "". */
static esp_err_t send_state(httpd_req_t *req, const char *extra)
{
    /* Read the generation first. If the list changes after this, the
     * page has an old generation, and its next change gets 409. */
    uint32_t gen = vault_generation();
    size_t size = 448 + VAULT_MAX * (2 * VAULT_LABEL_MAX + 3) + strlen(extra);
    char label[VAULT_LABEL_MAX + 1];
    char *buf = malloc(size);
    esp_err_t err;
    int n;

    if (!buf) {
        return send_error(req, ST_ERROR, "No memory.");
    }
    n = snprintf(buf, size,
                 "{\"version\":\"%s\",\"gen\":%lu,\"left\":%lu,\"token\":\"%s\",\"max\":%d,"
                 "\"labelMax\":%d,\"secretMax\":%d,\"genMin\":%d,\"genDefault\":%d,"
                 "\"backup\":%s,\"passMin\":%d,\"passMax\":%d,\"entries\":[",
                 esp_app_get_description()->version, (unsigned long)gen, (unsigned long)web_left_s(),
                 s_token, VAULT_MAX, VAULT_LABEL_MAX, VAULT_SECRET_MAX, RNG_PASSWORD_MIN,
                 RNG_PASSWORD_DEFAULT, s_backup_ok ? "true" : "false", BACKUP_PASS_MIN, BACKUP_PASS_MAX);
    for (int i = 0; vault_label(i, label, sizeof(label)) == ESP_OK; i++) {
        if (i > 0) {
            buf[n++] = ',';
        }
        buf[n++] = '"';
        for (const char *p = label; *p; p++) {
            if (*p == '"' || *p == '\\') {
                buf[n++] = '\\';
            }
            buf[n++] = *p;
        }
        buf[n++] = '"';
    }
    snprintf(buf + n, size - n, "]%s}", extra);
    err = send_json(req, ST_OK, buf);
    free(buf);
    return err;
}

/* Send the state after a change, or the error as a message. */
static esp_err_t reply(httpd_req_t *req, esp_err_t err, const char *what)
{
    switch (err) {
    case ESP_OK:
        return send_state(req, "");
    case ESP_ERR_NOT_FOUND:
        return send_error(req, ST_CONFLICT, "No entry has this number. Check the list.");
    case ESP_ERR_INVALID_STATE:
        return send_error(req, ST_CONFLICT, "The entries changed. Check the list and try again.");
    case ESP_ERR_NO_MEM:
        return send_error(req, ST_BAD, "The vault is full (%d entries).", VAULT_MAX);
    case ESP_ERR_INVALID_SIZE:
        return send_error(req, ST_BAD, "The %s must have 1 to %d characters.", what,
                          strcmp(what, "label") == 0 ? VAULT_LABEL_MAX : VAULT_SECRET_MAX);
    case ESP_ERR_INVALID_ARG:
        return send_error(req, ST_BAD, "The %s must contain only printable ASCII characters.", what);
    default:
        return send_error(req, ST_ERROR, "Error: %s.", esp_err_to_name(err));
    }
}

static esp_err_t redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* iOS needs a body to detect the captive portal. */
    return httpd_resp_send(req, "hw-pass", HTTPD_RESP_USE_STRLEN);
}

/* ---- Requests ---- */

/* True if the request is for WEB_IP. Then it counts as page activity. */
static bool own_host(httpd_req_t *req)
{
    char host[24];

    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host, WEB_IP) != 0 && strcmp(host, WEB_IP ":80") != 0)) {
        return false;
    }
    s_last_ms = now_ms();
    return true;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Copy the URL-decoded value of key from the form body to buf. Return
 * false if the key is missing, the value is too long, or it has a NUL.
 * After false, buf is zero: it can contain part of a password. */
static bool field(const char *body, const char *key, char *buf, size_t size)
{
    size_t klen = strlen(key);

    for (const char *p = body; *p;) {
        const char *end = p + strcspn(p, "&");

        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t n = 0;

            for (p += klen + 1; p < end; p++) {
                char c = *p;

                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && end - p > 2 && hex(p[1]) >= 0 && hex(p[2]) >= 0) {
                    c = (char)(hex(p[1]) << 4 | hex(p[2]));
                    p += 2;
                }
                if (c == '\0' || n + 1 >= size) {
                    vault_zero(buf, size);
                    return false;
                }
                buf[n++] = c;
            }
            buf[n] = '\0';
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

/* Return the number in the field key, or -1. */
static int field_int(const char *body, const char *key)
{
    char buf[8], *end;
    long v;

    if (!field(body, key, buf, sizeof(buf))) {
        return -1;
    }
    v = strtol(buf, &end, 10);
    return end != buf && *end == '\0' && v >= 0 && v <= 0xFFFF ? (int)v : -1;
}

/* True if the page shows the current list. Call it between vault_hold
 * and vault_release, together with the change of the entry. */
static bool same_generation(const char *body)
{
    char buf[12], *end;
    unsigned long v;

    if (!field(body, "gen", buf, sizeof(buf))) {
        return false;
    }
    v = strtoul(buf, &end, 10);
    return end != buf && *end == '\0' && v == vault_generation();
}

static esp_err_t bad_field(httpd_req_t *req, const char *what)
{
    return send_error(req, ST_BAD, "The %s is missing or too long.", what);
}

/* Read the form body. Return NULL after an error. */
static char *read_body(httpd_req_t *req)
{
    size_t len = req->content_len, got = 0;
    int tries = 0;
    char *body;

    if (len > BODY_MAX) {
        send_error(req, ST_TOO_LARGE, "The request is too large.");
        return NULL;
    }
    body = malloc(len + 1);
    if (!body) {
        send_error(req, ST_ERROR, "No memory.");
        return NULL;
    }
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++tries < RECV_TRIES) {
            continue;
        }
        if (r <= 0) {
            vault_zero(body, got);
            free(body);
            return NULL;
        }
        got += r;
    }
    body[len] = '\0';
    return body;
}

/* ---- Actions. Each one sends the response. ---- */

static esp_err_t act_add(httpd_req_t *req, const char *body)
{
    char label[FIELD_MAX], secret[FIELD_MAX], len_text[8];
    esp_err_t err;

    if (!field(body, "label", label, sizeof(label))) {
        return bad_field(req, "label");
    }
    err = vault_check_text(label, VAULT_LABEL_MAX);
    if (err != ESP_OK) {
        return reply(req, err, "label");
    }
    /* With "len", make a random password. */
    if (field(body, "len", len_text, sizeof(len_text))) {
        int len = field_int(body, "len");

        if (len < RNG_PASSWORD_MIN || len > VAULT_SECRET_MAX) {
            return send_error(req, ST_BAD, "The length must be %d to %d.", RNG_PASSWORD_MIN, VAULT_SECRET_MAX);
        }
        rng_password(secret, len);
    } else if (!field(body, "secret", secret, sizeof(secret))) {
        return bad_field(req, "password");
    }
    err = vault_add(label, secret);
    vault_zero(secret, sizeof(secret));
    return reply(req, err, "password");
}

static esp_err_t act_label(httpd_req_t *req, const char *body)
{
    char label[FIELD_MAX];

    esp_err_t err;

    if (!field(body, "label", label, sizeof(label))) {
        return bad_field(req, "label");
    }
    vault_hold();
    err = same_generation(body) ? vault_set_label(field_int(body, "i"), label) : ESP_ERR_INVALID_STATE;
    vault_release();
    return reply(req, err, "label");
}

static esp_err_t act_secret(httpd_req_t *req, const char *body)
{
    char secret[FIELD_MAX];
    esp_err_t err;

    if (!field(body, "secret", secret, sizeof(secret))) {
        return bad_field(req, "password");
    }
    vault_hold();
    err = same_generation(body) ? vault_set_secret(field_int(body, "i"), secret) : ESP_ERR_INVALID_STATE;
    vault_release();
    vault_zero(secret, sizeof(secret));
    return reply(req, err, "password");
}

static esp_err_t act_move(httpd_req_t *req, const char *body)
{
    esp_err_t err;

    vault_hold();
    err = same_generation(body) ? vault_move(field_int(body, "i"), field_int(body, "to")) : ESP_ERR_INVALID_STATE;
    vault_release();
    return reply(req, err, "");
}

static esp_err_t act_delete(httpd_req_t *req, const char *body)
{
    esp_err_t err;

    vault_hold();
    err = same_generation(body) ? vault_delete(field_int(body, "i")) : ESP_ERR_INVALID_STATE;
    vault_release();
    return reply(req, err, "");
}

static esp_err_t act_wipe(httpd_req_t *req, const char *body)
{
    char confirm[4];

    if (!field(body, "confirm", confirm, sizeof(confirm)) || strcmp(confirm, "YES") != 0) {
        return send_error(req, ST_BAD, "Type YES to erase all entries.");
    }
    return reply(req, vault_wipe(), "");
}

/* Check one import line "label,password". Copy the label to label.
 * Return the password, or NULL and the problem in msg. */
static const char *import_line(const char *s, char *label, char *msg, size_t size)
{
    const char *comma = strchr(s, ','), *a = s, *b, *secret;
    esp_err_t err;

    if (!comma) {
        snprintf(msg, size, "use the form label,password");
        return NULL;
    }
    while (a < comma && *a == ' ') {
        a++;
    }
    for (b = comma; b > a && b[-1] == ' '; b--) {
    }
    secret = comma + 1;
    err = b - a > VAULT_LABEL_MAX ? ESP_ERR_INVALID_SIZE : ESP_OK;
    if (err == ESP_OK) {
        memcpy(label, a, b - a);
        label[b - a] = '\0';
        err = vault_check_text(label, VAULT_LABEL_MAX);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        snprintf(msg, size, "the label must have 1 to %d characters", VAULT_LABEL_MAX);
        return NULL;
    }
    if (err != ESP_OK) {
        snprintf(msg, size, "the label must contain only printable ASCII characters");
        return NULL;
    }
    err = vault_check_text(secret, VAULT_SECRET_MAX);
    if (err == ESP_ERR_INVALID_SIZE) {
        snprintf(msg, size, "the password must have 1 to %d characters", VAULT_SECRET_MAX);
        return NULL;
    }
    if (err != ESP_OK) {
        snprintf(msg, size, "the password must contain only printable ASCII characters");
        return NULL;
    }
    /* Usually a mistake, for example "label, password". */
    if (secret[0] == ' ' || secret[strlen(secret) - 1] == ' ') {
        snprintf(msg, size, "the password starts or ends with a space. Use Add for it");
        return NULL;
    }
    return secret;
}

/* Lines of "label,password". If a line is wrong, nothing changes. */
static esp_err_t act_import(httpd_req_t *req, const char *body)
{
    size_t size = strlen(body) + 1;     /* The decoded text is not longer. */
    char labels[VAULT_MAX][VAULT_LABEL_MAX + 1], problem[80], msg[128] = "";
    const char *label_ptr[VAULT_MAX], *secret_ptr[VAULT_MAX], *secret;
    char *data = malloc(size), *w, *end;
    int count = 0, line = 0;
    esp_err_t err = ESP_OK;

    if (!data) {
        return send_error(req, ST_ERROR, "No memory.");
    }
    if (!field(body, "data", data, size)) {
        free(data);
        return bad_field(req, "text");
    }
    /* Remove CR, and make each line a string. */
    w = data;
    for (const char *r = data; *r; r++) {
        if (*r != '\r') {
            *w++ = *r == '\n' ? '\0' : *r;
        }
    }
    end = w;
    *w = '\0';

    for (char *s = data; s < end && !msg[0]; s += strlen(s) + 1) {
        line++;
        if (*s == '\0') {
            continue;
        }
        if (count == VAULT_MAX) {
            snprintf(msg, sizeof(msg), "The text has more than %d entries.", VAULT_MAX);
        } else if ((secret = import_line(s, labels[count], problem, sizeof(problem)))) {
            label_ptr[count] = labels[count];
            secret_ptr[count++] = secret;
        } else {
            snprintf(msg, sizeof(msg), "Line %d: %s.", line, problem);
        }
    }
    if (!msg[0] && count == 0) {
        snprintf(msg, sizeof(msg), "There is nothing to import.");
    }
    if (!msg[0]) {
        /* One write: if it fails, the vault keeps the old entries. */
        err = vault_add_many(label_ptr, secret_ptr, count);
        if (err == ESP_ERR_NO_MEM) {
            snprintf(msg, sizeof(msg), "The text has %d entries. The vault has space for %d.",
                     count, VAULT_MAX - vault_count());
        } else if (err != ESP_OK) {
            snprintf(msg, sizeof(msg), "Cannot save: %s. Nothing changed.", esp_err_to_name(err));
        }
    }
    vault_zero(data, size);
    free(data);
    if (msg[0]) {
        return send_error(req, err == ESP_OK || err == ESP_ERR_NO_MEM ? ST_BAD : ST_ERROR, "%s", msg);
    }
    return send_state(req, "");
}

/* Encrypted backup of all entries, in the member "file" of the state. */
static esp_err_t act_backup(httpd_req_t *req, const char *body)
{
    char pass[FIELD_MAX], *text, *json, *w;
    size_t lines = 0;
    esp_err_t err;

    if (!s_backup_ok) {
        return send_error(req, ST_FORBIDDEN, "Start the setup mode with the buttons on the device to make a backup.");
    }
    if (!field(body, "pass", pass, sizeof(pass))) {
        return bad_field(req, "passphrase");
    }
    err = backup_export(pass, &text);
    vault_zero(pass, sizeof(pass));
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, ST_BAD, "The passphrase must have %d to %d characters.", BACKUP_PASS_MIN, BACKUP_PASS_MAX);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, ST_BAD, "The passphrase must contain only printable ASCII characters.");
    }
    if (err != ESP_OK) {
        return send_error(req, ST_ERROR, "Cannot make the backup: %s.", esp_err_to_name(err));
    }
    /* Base64 needs no JSON escape. Only the line feeds. */
    for (const char *r = text; *r; r++) {
        lines += *r == '\n';
    }
    json = malloc(strlen(text) + lines + 16);
    if (!json) {
        free(text);
        return send_error(req, ST_ERROR, "No memory.");
    }
    w = json + sprintf(json, ",\"file\":\"");
    for (const char *r = text; *r; r++) {
        if (*r == '\n') {
            *w++ = '\\';
            *w++ = 'n';
        } else {
            *w++ = *r;
        }
    }
    strcpy(w, "\"");
    free(text);
    err = send_state(req, json);
    free(json);
    return err;
}

static esp_err_t act_done(httpd_req_t *req, const char *body)
{
    esp_err_t err = send_json(req, ST_OK, "{}");

    s_done = true;  /* After the response: the UI then stops the server. */
    return err;
}

/* The actions with an entry number also check the generation of the page. */
static const struct {
    const char *name;
    esp_err_t (*fn)(httpd_req_t *req, const char *body);
} s_actions[] = {
    { "add", act_add },
    { "label", act_label },
    { "secret", act_secret },
    { "move", act_move },
    { "delete", act_delete },
    { "wipe", act_wipe },
    { "import", act_import },
    { "backup", act_backup },
    { "done", act_done },
};

static esp_err_t api_post(httpd_req_t *req)
{
    const char *name = req->uri + strlen("/api/");
    size_t name_len = strcspn(name, "?");
    char token[TOKEN_LEN + 1];
    esp_err_t err;
    char *body;
    size_t i;

    if (!own_host(req)) {
        return send_error(req, ST_FORBIDDEN, "Use %s.", s_portal);
    }
    if (httpd_req_get_hdr_value_str(req, "X-Token", token, sizeof(token)) != ESP_OK ||
        strcmp(token, s_token) != 0) {
        return send_error(req, ST_FORBIDDEN, "The session is not valid. Load the page again.");
    }
    for (i = 0; i < sizeof(s_actions) / sizeof(s_actions[0]); i++) {
        if (strlen(s_actions[i].name) == name_len && strncmp(name, s_actions[i].name, name_len) == 0) {
            break;
        }
    }
    if (i == sizeof(s_actions) / sizeof(s_actions[0])) {
        return send_error(req, "404 Not Found", "Unknown action.");
    }
    body = read_body(req);
    if (!body) {
        return ESP_FAIL;    /* Close the connection. */
    }
    err = s_actions[i].fn(req, body);
    vault_zero(body, req->content_len);
    free(body);
    return err;
}

static esp_err_t state_get(httpd_req_t *req)
{
    if (!own_host(req)) {
        return send_error(req, ST_FORBIDDEN, "Use %s.", s_portal);
    }
    return send_state(req, "");
}

static esp_err_t page_get(httpd_req_t *req)
{
    if (!own_host(req)) {
        return redirect(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy", s_csp);
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    return httpd_resp_send(req, s_page_start, s_page_end - s_page_start);
}

/* Unknown paths. Requests for other hosts are connectivity checks of the
 * device: the redirect makes it show the captive portal. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    if (!own_host(req)) {
        return redirect(req);
    }
    return send_error(req, "404 Not Found", "Not found.");
}

/* ---- Start and stop ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Device joined");
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        const ip_event_ap_staipassigned_t *e = data;

        ESP_LOGI(TAG, "Device address " IPSTR, IP2STR(&e->ip));
        s_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;

        ESP_LOGI(TAG, "Device left, reason %u", e->reason);
        s_connected = false;    /* Only one device can connect. */
    }
}

static esp_err_t init_once(void)
{
    static bool done;
    esp_err_t err;

    if (done) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* After an error, the next start continues here. A second registration
     * of a handler replaces the first. */
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "AP netif");
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_event, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_event, NULL), TAG, "event");
    /* The log goes to the command line. Keep the start quiet, and stop
     * the warnings about the requests of connectivity checks. */
    for (size_t i = 0; i < sizeof(s_quiet) / sizeof(s_quiet[0]); i++) {
        esp_log_level_set(s_quiet[i], ESP_LOG_WARN);
    }
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    done = true;
    return ESP_OK;
}

static esp_err_t start_wifi(const web_info_t *info)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t cfg = {
        .ap = {
            .ssid_len = strlen(info->ssid),
            .channel = AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .max_connection = 1,    /* PMF is optional: WPA2 devices can connect. */
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    esp_err_t err;
    bool radio;

    memcpy(cfg.ap.ssid, info->ssid, cfg.ap.ssid_len);
    memcpy(cfg.ap.password, info->password, strlen(info->password));

    rng_radio_lock();
    err = esp_wifi_init(&init);
    radio = err == ESP_OK;  /* Until esp_wifi_deinit: the PHY can be on. */
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    rng_radio_unlock(radio);
    vault_zero(&cfg, sizeof(cfg));
    ESP_RETURN_ON_ERROR(err, TAG, "Wi-Fi start");

    if (esp_wifi_set_max_tx_power(AP_TX_POWER) != ESP_OK) {
        ESP_LOGW(TAG, "cannot set the TX power");
    }
    return ESP_OK;
}

static esp_err_t start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page_get },
        { .uri = "/api/state", .method = HTTP_GET, .handler = state_get },
        { .uri = "/api/*", .method = HTTP_POST, .handler = api_post },
    };

    cfg.max_open_sockets = 5;   /* LWIP_MAX_SOCKETS is 10: 3 for httpd, 1 for DNS. */
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd");
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &uris[i]), TAG, "URI");
    }
    return httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
}

esp_err_t web_start(web_info_t *info, bool buttons)
{
    esp_err_t err;

    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_backup_ok = buttons;
    /* Make the secrets before the radio starts: the ADC noise source is free. */
    strcpy(info->ssid, "hw-pass-");
    rng_text(info->ssid + strlen(info->ssid), 4, s_upper);
    rng_text(info->password, WEB_PASSWORD_LEN, s_lower);
    rng_text(s_token, TOKEN_LEN, s_lower);
    s_done = false;
    s_connected = false;
    s_last_ms = now_ms();

    err = init_once();
    if (err == ESP_OK) {
        err = start_wifi(info);
    }
    if (err == ESP_OK) {
        err = dns_start(esp_ip4addr_aton(WEB_IP));
    }
    if (err == ESP_OK) {
        err = start_server();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        web_stop();
        return err;
    }
    s_info = *info;
    __sync_synchronize();   /* Other tasks see s_info before s_active. */
    s_active = true;
    ESP_LOGI(TAG, "Wi-Fi setup mode on: %s", info->ssid);
    return ESP_OK;
}

void web_stop(void)
{
    esp_err_t err;

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    dns_stop();
    rng_radio_lock();
    esp_wifi_stop();
    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "Wi-Fi stop: %s", esp_err_to_name(err));
    }
    rng_radio_unlock(err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT);
    vault_zero(s_token, sizeof(s_token));
    vault_zero(&s_info, sizeof(s_info));
    s_connected = false;
    s_backup_ok = false;
    ESP_LOGI(TAG, "Wi-Fi setup mode off");
    s_active = false;   /* Last: the command line waits for this. */
}

#else   /* CONFIG_HWPASS_WEB */

esp_err_t web_start(web_info_t *info, bool buttons)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void web_stop(void)
{
}

bool web_active(void)
{
    return false;
}

esp_err_t web_get_info(web_info_t *info)
{
    return ESP_ERR_INVALID_STATE;
}

bool web_connected(void)
{
    return false;
}

uint32_t web_left_s(void)
{
    return 0;
}

#endif  /* CONFIG_HWPASS_WEB */
