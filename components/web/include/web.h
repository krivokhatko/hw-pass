#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_IP              "192.168.4.1"
#define WEB_PASSWORD_LEN    16

/* Network name and password of the access point. Each start makes new ones. */
typedef struct {
    char ssid[16];                          /* "hw-pass-XXXX" */
    char password[WEB_PASSWORD_LEN + 1];
} web_info_t;

/* Wi-Fi setup mode: an access point with a web page that changes the
 * entries. Call web_start and web_stop from one task only. */

/* Start the access point, the DNS server, and the web server. Returns
 * ESP_ERR_NOT_SUPPORTED if the build has no Wi-Fi (CONFIG_HWPASS_WEB). */
esp_err_t web_start(web_info_t *info);

/* Stop all and turn the radio off. Safe to call when the mode is off. */
void web_stop(void);

bool web_active(void);

/* Copy the network name and password. ESP_ERR_INVALID_STATE if the mode is off. */
esp_err_t web_get_info(web_info_t *info);

/* A device is connected and has an IP address. */
bool web_connected(void);

/* Seconds until the timeout: 5 min after the last request from the page.
 * 0 when the user pushed "Done" on the page. Then stop the mode. */
uint32_t web_left_s(void);

#ifdef __cplusplus
}
#endif
