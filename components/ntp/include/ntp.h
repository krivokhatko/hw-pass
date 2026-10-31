#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Time sync with the NTP server of the Wi-Fi router. The device joins
 * the Wi-Fi network as a station for some seconds, gets the time, sets
 * the clock, and turns the radio off again. All functions are safe to
 * call from different tasks. */

#define NTP_SSID_MAX    32
#define NTP_PASS_MIN    8       /* WPA2 and WPA3 passwords */
#define NTP_PASS_MAX    64
#define NTP_SERVER_MAX  63
#define NTP_RESULT_TEXT 128

/* Start the sync task. With a network and the automatic sync on, it
 * syncs 20 s after the start, then each 24 h. After a failed sync, it
 * tries again after 10 min, then after 2 times this time, up to 4 h.
 * Call after clock_init. */
esp_err_t ntp_init(void);

/* The network for the sync. The device joins only a WPA2 or WPA3
 * network: the password has NTP_PASS_MIN to NTP_PASS_MAX characters.
 * ESP_ERR_INVALID_SIZE: the name or the password has a wrong length.
 * ESP_ERR_NOT_SUPPORTED: the build has no time sync (CONFIG_HWPASS_NTP). */
esp_err_t ntp_set_wifi(const char *ssid, const char *password);

/* Remove the network and its password. Then no sync comes. */
void ntp_clear_wifi(void);

/* Copy the network name. "" if no network is set. */
void ntp_get_ssid(char *buf, size_t size);

/* The NTP server: a host name or an IP address. "" means the router, the
 * default gateway. ESP_ERR_INVALID_SIZE: longer than NTP_SERVER_MAX. */
esp_err_t ntp_set_server(const char *host);
void ntp_get_server(char *buf, size_t size);

/* The automatic sync. Default on. */
bool ntp_auto(void);
esp_err_t ntp_set_auto(bool on);

/* Sync now, in the sync task. The result comes when ntp_count changes.
 * ESP_OK also when a sync runs already. ESP_ERR_NOT_FOUND: no network.
 * ESP_ERR_INVALID_STATE: the Wi-Fi setup mode is on. */
esp_err_t ntp_request(void);

/* A sync runs. */
bool ntp_busy(void);

/* Number of syncs that ended since the start, with or without success. */
uint32_t ntp_count(void);

/* Text of the last result, for example "OK, 3 h ago: set from
 * 192.168.1.1, correction +0.4 s" or "failed: no reply from 192.168.1.1".
 * "" if no sync ended yet. */
void ntp_result_text(char *buf, size_t size);

/* The Wi-Fi setup mode needs the radio. hold = true stops a sync that
 * runs, waits for its end, and starts no new sync until hold = false. */
void ntp_hold(bool hold);

#ifdef __cplusplus
}
#endif
