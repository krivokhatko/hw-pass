/* Time sync with the NTP server of the Wi-Fi router (SNTP, RFC 4330).
 * The sync task joins the Wi-Fi network as a station, sends one request
 * to the router or to a set server, sets the clock from the reply, and
 * turns the radio off again. A sync takes some seconds.
 *
 * Protection:
 * - The device joins only a network with WPA2 or WPA3. It does not join
 *   an open or a WEP network with the same name.
 * - It opens no port. It sends the request from a random port and
 *   accepts only a reply from the address and the port of the server.
 * - The request has a random 64-bit value in the transmit timestamp. The
 *   reply must return it in the originate timestamp: a false reply must
 *   guess it.
 * - The reply must come from a server (mode 4) of version 3 or 4, with a
 *   stratum of 1 to 15 and no alarm. clock_set_utc checks the year.
 * - Only the sync task uses the Wi-Fi driver. The Wi-Fi setup mode holds
 *   the task while it runs.
 * - The password stays in NVS. The task reads it for the driver and sets
 *   its copy to zero. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "sdkconfig.h"
#include "ntp.h"

#if CONFIG_HWPASS_NTP

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "clock.h"
#include "rng.h"
#include "vault.h"

#define NAMESPACE       "ntp"
#define HOSTNAME        "hw-pass"       /* The name in the client list of the router */
#define TASK_STACK      5120
#define START_DELAY_US  (20 * 1000000LL)
#define PERIOD_US       (24 * 3600 * 1000000LL)
#define RETRY_MIN_US    (10 * 60 * 1000000LL)
#define RETRY_MAX_US    (4 * 3600 * 1000000LL)
#define HELD_RETRY_US   (60 * 1000000LL)    /* After a stop by the setup mode */
#define CONNECT_MS      15000
#define CONNECT_TRIES   2
#define REPLY_MS        3000
#define REPLY_TRIES     3
#define HOLD_WAIT_MS    30000
#define NTP_PORT        123
#define NTP_EPOCH       2208988800u         /* 1970-01-01, in seconds since 1900 */
#define NTP_PACKET      48

typedef enum {
    RESULT_NONE,
    RESULT_OK,
    RESULT_HELD,        /* The Wi-Fi setup mode stopped the sync. */
    RESULT_NOT_FOUND,   /* The network is not found, or it has no WPA2. */
    RESULT_PASSWORD,    /* The network refused the password. */
    RESULT_NO_LINK,     /* No connection for another reason: s_reason */
    RESULT_NO_IP,       /* Connected, but no IP address */
    RESULT_NO_SERVER,   /* No gateway, or the server name is not found. */
    RESULT_NO_REPLY,
    RESULT_BAD_REPLY,
    RESULT_ERROR,       /* An error of the driver: s_err */
} result_t;

static const char *TAG = "ntp";
static const char *const s_quiet[] = { "wifi", "wifi_init", "phy_init", "pp", "net80211", "esp_netif_lwip" };

static SemaphoreHandle_t s_lock;            /* s_ssid, s_server */
static TaskHandle_t s_task;
static esp_netif_t *s_netif;
static char s_ssid[NTP_SSID_MAX + 1];       /* "" means: no network. */
static char s_server[NTP_SERVER_MAX + 1];   /* "" means: the router. */
static volatile bool s_auto = true;
static volatile bool s_hold;                /* The Wi-Fi setup mode needs the radio. */
static volatile bool s_busy;                /* A sync runs. */
static volatile bool s_request;
static volatile uint32_t s_count;           /* Syncs that ended */
static volatile bool s_connected, s_got_ip, s_disconnected;
static volatile uint8_t s_reason;           /* Of the last disconnect */
static int64_t s_next_us;                   /* Time of the next automatic sync */
static int64_t s_retry_us = RETRY_MIN_US;
/* The last result */
static result_t s_result;
static int64_t s_done_us;
static bool s_had_time;                     /* The time was set before the sync. */
static int64_t s_correction_ms;
static esp_err_t s_err;
static char s_from[16];                     /* IP address of the server */

/* ---- Settings in NVS ---- */

static esp_err_t save_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = nvs_set_str(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

static esp_err_t save_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

static void load(void)
{
    nvs_handle_t h;
    size_t size;
    uint8_t on = 1;

    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size = sizeof(s_ssid);
        if (nvs_get_str(h, "ssid", s_ssid, &size) != ESP_OK) {
            s_ssid[0] = '\0';
        }
        size = sizeof(s_server);
        if (nvs_get_str(h, "server", s_server, &size) != ESP_OK) {
            s_server[0] = '\0';
        }
        nvs_get_u8(h, "auto", &on);
        nvs_close(h);
    }
    s_auto = on != 0;
}

static bool configured(void)
{
    bool set;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set = s_ssid[0] != '\0';
    xSemaphoreGive(s_lock);
    return set;
}

/* ---- Wi-Fi station ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        s_disconnected = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_got_ip = true;
    }
}

static esp_err_t init_once(void)
{
    static bool done;
    esp_err_t err;

    if (done) {
        return ESP_OK;
    }
    /* The web module can do these two steps first. Both allow a second call. */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "station netif");
        esp_netif_set_hostname(s_netif, HOSTNAME);
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_event, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_event, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "event");
    /* The log goes to the command line. Keep the start quiet. */
    for (size_t i = 0; i < sizeof(s_quiet) / sizeof(s_quiet[0]); i++) {
        esp_log_level_set(s_quiet[i], ESP_LOG_WARN);
    }
    done = true;
    return ESP_OK;
}

static result_t start_wifi(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,   /* Not an open, WEP, or WPA network with this name */
            .pmf_cfg.capable = true,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    char pass[NTP_PASS_MAX + 1];
    size_t size = sizeof(pass);
    nvs_handle_t h;
    esp_err_t err;
    bool radio;

    err = init_once();
    if (err == ESP_OK) {
        err = nvs_open(NAMESPACE, NVS_READONLY, &h);
        if (err == ESP_OK) {
            err = nvs_get_str(h, "pass", pass, &size);
            nvs_close(h);
        }
    }
    if (err != ESP_OK) {
        s_err = err;
        return RESULT_ERROR;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(cfg.sta.ssid, s_ssid, strlen(s_ssid));
    xSemaphoreGive(s_lock);
    memcpy(cfg.sta.password, pass, strlen(pass));
    vault_zero(pass, sizeof(pass));
    s_connected = s_got_ip = s_disconnected = false;

    rng_radio_lock();
    err = esp_wifi_init(&init);
    radio = err == ESP_OK;  /* Until esp_wifi_deinit: the PHY can be on. */
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    if (err == ESP_OK) {
        /* No modem sleep: the session is short, and a reply must not wait
         * for a beacon while Bluetooth also uses the radio. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        err = esp_wifi_start();     /* WIFI_EVENT_STA_START: on_event connects. */
    }
    rng_radio_unlock(RNG_RADIO_WIFI, radio);
    vault_zero(&cfg, sizeof(cfg));
    if (err != ESP_OK) {
        s_err = err;
        return RESULT_ERROR;
    }
    return RESULT_OK;
}

static void stop_wifi(void)
{
    esp_err_t err;

    esp_wifi_disconnect();
    rng_radio_lock();
    esp_wifi_stop();
    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "Wi-Fi stop: %s", esp_err_to_name(err));
    }
    rng_radio_unlock(RNG_RADIO_WIFI, err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT);
}

static bool not_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static bool bad_password(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_EXPIRE || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

/* Wait for an IP address. After a disconnect, connect one more time. */
static result_t wait_ip(void)
{
    int tries = 1;

    for (int ms = 0; ms < CONNECT_MS; ms += 100) {
        if (s_hold) {
            return RESULT_HELD;
        }
        if (s_got_ip) {
            return RESULT_OK;
        }
        if (s_disconnected) {
            uint8_t reason = s_reason;

            if (bad_password(reason)) {
                return RESULT_PASSWORD;
            }
            if (tries >= CONNECT_TRIES) {
                return not_found(reason) ? RESULT_NOT_FOUND : RESULT_NO_LINK;
            }
            tries++;
            s_disconnected = false;
            s_connected = false;
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_reason = 0;
    return s_connected ? RESULT_NO_IP : RESULT_NO_LINK;
}

/* The address of the server: the gateway, or the set host. */
static result_t find_server(struct sockaddr_in *server)
{
    char host[NTP_SERVER_MAX + 1];
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;

    memset(server, 0, sizeof(*server));
    server->sin_family = AF_INET;
    server->sin_port = htons(NTP_PORT);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(host, s_server);
    xSemaphoreGive(s_lock);
    if (host[0] == '\0') {
        esp_netif_ip_info_t ip;

        if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK || ip.gw.addr == 0) {
            return RESULT_NO_SERVER;
        }
        server->sin_addr.s_addr = ip.gw.addr;
        return RESULT_OK;
    }
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return RESULT_NO_SERVER;
    }
    server->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return RESULT_OK;
}

/* ---- SNTP ---- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* An NTP timestamp as microseconds since 1970. */
static int64_t unix_us(const uint8_t *p)
{
    uint32_t sec = be32(p) - NTP_EPOCH;     /* Wraps right from 2036 to 2106. */

    return (int64_t)sec * 1000000 + (int64_t)(((uint64_t)be32(p + 4) * 1000000) >> 32);
}

/* Check a reply and compute the time at its arrival. t1 and t4 are the
 * times of the request and the reply on the device, in microseconds of
 * esp_timer. */
static result_t check_reply(const uint8_t *p, int n, const uint8_t *nonce, int64_t t1, int64_t t4,
                            struct timeval *now)
{
    uint8_t li = p[0] >> 6, vn = (p[0] >> 3) & 7, mode = p[0] & 7, stratum = p[1];
    int64_t t2, t3, delay;

    if (n < NTP_PACKET || mode != 4 || vn < 3 || vn > 4 || li == 3 || stratum < 1 || stratum > 15 ||
        memcmp(p + 24, nonce, 8) != 0 || (be32(p + 40) == 0 && be32(p + 44) == 0)) {
        return RESULT_BAD_REPLY;
    }
    t2 = unix_us(p + 32);   /* The request came to the server. */
    t3 = unix_us(p + 40);   /* The reply left the server. */
    /* RFC 4330: the time at the arrival is t3 plus half of the network delay. */
    delay = (t4 - t1) - (t3 - t2);
    if (delay < 0 || delay > t4 - t1) {
        delay = t4 - t1;
    }
    t3 += delay / 2;
    now->tv_sec = t3 / 1000000;
    now->tv_usec = t3 % 1000000;
    return RESULT_OK;
}

/* One request and its reply. */
static result_t exchange(const struct sockaddr_in *server, struct timeval *now)
{
    uint8_t req[NTP_PACKET] = { 0x23 };     /* Leap 0, version 4, mode 3: client */
    uint8_t rep[NTP_PACKET + 32];           /* A reply can have an authenticator. */
    uint8_t nonce[8];
    struct sockaddr_in from;
    socklen_t from_len;
    struct timeval timeout = { .tv_sec = REPLY_MS / 1000 };
    result_t r = RESULT_NO_REPLY;
    int64_t t1, t4;
    int sock, n;

    rng_bytes(nonce, sizeof(nonce));
    memcpy(req + 40, nonce, sizeof(nonce));     /* Transmit timestamp: the nonce, not a time */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        s_err = ESP_FAIL;
        return RESULT_ERROR;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    t1 = esp_timer_get_time();
    if (sendto(sock, req, sizeof(req), 0, (const struct sockaddr *)server, sizeof(*server)) == sizeof(req)) {
        while (esp_timer_get_time() - t1 < REPLY_MS * 1000LL) {
            from_len = sizeof(from);
            n = recvfrom(sock, rep, sizeof(rep), 0, (struct sockaddr *)&from, &from_len);
            t4 = esp_timer_get_time();
            if (n < 0) {
                break;  /* Timeout */
            }
            if (from.sin_addr.s_addr != server->sin_addr.s_addr || from.sin_port != server->sin_port) {
                continue;   /* Not from the server: ignore it. */
            }
            r = check_reply(rep, n, nonce, t1, t4, now);
            break;
        }
    }
    close(sock);
    return r;
}

static result_t set_time(const struct timeval *now)
{
    struct timeval before;
    bool valid = clock_valid();

    gettimeofday(&before, NULL);
    if (clock_set_utc(now) != ESP_OK) {
        return RESULT_BAD_REPLY;    /* A time outside CLOCK_MIN_YEAR to 2099 */
    }
    s_had_time = valid;
    s_correction_ms = valid ? (now->tv_sec - before.tv_sec) * 1000 + (now->tv_usec - before.tv_usec) / 1000 : 0;
    return RESULT_OK;
}

static result_t sync_time(void)
{
    struct sockaddr_in server;
    struct timeval now;
    result_t r = start_wifi();

    if (r == RESULT_OK) {
        r = wait_ip();
    }
    if (r == RESULT_OK) {
        r = find_server(&server);
    }
    if (r == RESULT_OK) {
        inet_ntoa_r(server.sin_addr, s_from, sizeof(s_from));
        for (int i = 0; i < REPLY_TRIES; i++) {
            r = exchange(&server, &now);
            if (r != RESULT_NO_REPLY || s_hold) {
                break;
            }
        }
    }
    if (r == RESULT_OK) {
        r = set_time(&now);
    }
    stop_wifi();
    return r;
}

/* ---- Task ---- */

static void run(void)
{
    char text[NTP_RESULT_TEXT];
    result_t r;
    int64_t now;

    s_busy = true;
    __sync_synchronize();   /* Pairs with the barrier in ntp_hold. */
    if (s_hold) {
        s_busy = false;
        return;
    }
    r = sync_time();
    now = esp_timer_get_time();
    s_result = r;
    s_done_us = now;
    if (r == RESULT_OK) {
        s_next_us = now + PERIOD_US;
        s_retry_us = RETRY_MIN_US;
    } else if (r == RESULT_HELD) {
        s_next_us = now + HELD_RETRY_US;
    } else {
        s_next_us = now + s_retry_us;
        s_retry_us = s_retry_us * 2 < RETRY_MAX_US ? s_retry_us * 2 : RETRY_MAX_US;
    }
    ntp_result_text(text, sizeof(text));
    if (r == RESULT_OK) {
        ESP_LOGI(TAG, "%s", text);
    } else {
        ESP_LOGW(TAG, "%s", text);
    }
    __sync_synchronize();   /* Other tasks see the result before the count. */
    s_count++;
    s_busy = false;
}

static void ntp_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_hold) {
            continue;
        }
        if (!s_request && !(s_auto && configured() && esp_timer_get_time() >= s_next_us)) {
            continue;
        }
        s_request = false;
        run();
    }
}

esp_err_t ntp_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");
    load();
    s_next_us = esp_timer_get_time() + START_DELAY_US;
    if (xTaskCreate(ntp_task, "ntp", TASK_STACK, NULL, 2, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- Settings ---- */

static bool printable(const char *s)
{
    for (; *s; s++) {
        if (*s < 0x20 || *s > 0x7E) {
            return false;
        }
    }
    return true;
}

esp_err_t ntp_set_wifi(const char *ssid, const char *password)
{
    size_t slen = strlen(ssid), plen = strlen(password);
    esp_err_t err;

    if (slen == 0 || slen > NTP_SSID_MAX || plen < NTP_PASS_MIN || plen > NTP_PASS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!printable(ssid) || !printable(password)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = save_str("pass", password);
    if (err == ESP_OK) {
        err = save_str("ssid", ssid);
    }
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_ssid, ssid);
    xSemaphoreGive(s_lock);
    s_retry_us = RETRY_MIN_US;
    s_next_us = esp_timer_get_time() + START_DELAY_US;  /* The user can sync now with "ntp" first. */
    return ESP_OK;
}

void ntp_clear_wifi(void)
{
    nvs_handle_t h;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ssid[0] = '\0';
    xSemaphoreGive(s_lock);
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
}

void ntp_get_ssid(char *buf, size_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, size, "%s", s_ssid);
    xSemaphoreGive(s_lock);
}

esp_err_t ntp_set_server(const char *host)
{
    esp_err_t err;

    if (strlen(host) > NTP_SERVER_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!printable(host)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = save_str("server", host);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_server, host);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void ntp_get_server(char *buf, size_t size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, size, "%s", s_server);
    xSemaphoreGive(s_lock);
}

bool ntp_auto(void)
{
    return s_auto;
}

esp_err_t ntp_set_auto(bool on)
{
    s_auto = on;
    return save_u8("auto", on);
}

/* ---- State ---- */

esp_err_t ntp_request(void)
{
    if (!s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!configured()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_hold) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        return ESP_OK;  /* The sync that runs gives the result. */
    }
    s_request = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool ntp_busy(void)
{
    return s_busy;
}

uint32_t ntp_count(void)
{
    uint32_t count = s_count;

    __sync_synchronize();   /* Pairs with the barrier in run. */
    return count;
}

/* ", 3 h ago" or "" in the first minute. */
static void ago_text(char *buf, size_t size)
{
    int64_t min = (esp_timer_get_time() - s_done_us) / 60000000LL;

    if (min < 1) {
        buf[0] = '\0';
    } else if (min < 60) {
        snprintf(buf, size, ", %lld min ago", (long long)min);
    } else if (min < 48 * 60) {
        snprintf(buf, size, ", %lld h ago", (long long)min / 60);
    } else {
        snprintf(buf, size, ", %lld d ago", (long long)min / (24 * 60));
    }
}

void ntp_result_text(char *buf, size_t size)
{
    char ago[20], ssid[NTP_SSID_MAX + 1], server[NTP_SERVER_MAX + 1];
    long long ms = s_correction_ms < 0 ? -s_correction_ms : s_correction_ms;

    if (s_result == RESULT_NONE) {
        buf[0] = '\0';
        return;
    }
    ago_text(ago, sizeof(ago));
    ntp_get_ssid(ssid, sizeof(ssid));
    ntp_get_server(server, sizeof(server));
    switch (s_result) {
    case RESULT_OK:
        if (s_had_time) {
            snprintf(buf, size, "OK%s: set from %s, correction %c%lld.%lld s", ago, s_from,
                     s_correction_ms < 0 ? '-' : '+', ms / 1000, ms % 1000 / 100);
        } else {
            snprintf(buf, size, "OK%s: set from %s", ago, s_from);
        }
        break;
    case RESULT_HELD:
        snprintf(buf, size, "stopped%s: the Wi-Fi setup mode started", ago);
        break;
    case RESULT_NOT_FOUND:
        snprintf(buf, size, "failed%s: the network \"%s\" is not found, or it has no WPA2 or WPA3", ago, ssid);
        break;
    case RESULT_PASSWORD:
        snprintf(buf, size, "failed%s: the network \"%s\" refused the password", ago, ssid);
        break;
    case RESULT_NO_LINK:
        snprintf(buf, size, "failed%s: no connection to \"%s\" (reason %u)", ago, ssid, s_reason);
        break;
    case RESULT_NO_IP:
        snprintf(buf, size, "failed%s: connected to \"%s\", but no IP address came", ago, ssid);
        break;
    case RESULT_NO_SERVER:
        if (server[0]) {
            snprintf(buf, size, "failed%s: the server \"%s\" is not found", ago, server);
        } else {
            snprintf(buf, size, "failed%s: the router gave no gateway address", ago);
        }
        break;
    case RESULT_NO_REPLY:
        snprintf(buf, size, "failed%s: no reply from %s", ago, s_from);
        break;
    case RESULT_BAD_REPLY:
        snprintf(buf, size, "failed%s: the reply from %s is not valid", ago, s_from);
        break;
    default:
        snprintf(buf, size, "failed%s: %s", ago, esp_err_to_name(s_err));
        break;
    }
}

void ntp_hold(bool hold)
{
    s_hold = hold;
    __sync_synchronize();   /* Pairs with the barrier in run. */
    for (int i = 0; hold && s_busy && i < HOLD_WAIT_MS / 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#else   /* CONFIG_HWPASS_NTP */

esp_err_t ntp_init(void)
{
    return ESP_OK;
}

esp_err_t ntp_set_wifi(const char *ssid, const char *password)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ntp_clear_wifi(void)
{
}

void ntp_get_ssid(char *buf, size_t size)
{
    buf[0] = '\0';
}

esp_err_t ntp_set_server(const char *host)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ntp_get_server(char *buf, size_t size)
{
    buf[0] = '\0';
}

bool ntp_auto(void)
{
    return false;
}

esp_err_t ntp_set_auto(bool on)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ntp_request(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ntp_busy(void)
{
    return false;
}

uint32_t ntp_count(void)
{
    return 0;
}

void ntp_result_text(char *buf, size_t size)
{
    buf[0] = '\0';
}

void ntp_hold(bool hold)
{
}

#endif  /* CONFIG_HWPASS_NTP */
