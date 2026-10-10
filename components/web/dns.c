/* DNS server for the captive portal. Each name has the address of the
 * access point. Phones then open the setup page by themselves. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "dns.h"

#define DNS_PORT        53
#define PACKET_MAX      512
#define HEADER_LEN      12
#define ANSWER_LEN      16
#define POLL_MS         200     /* dns_stop waits this time at most. */

static volatile bool s_run;
static SemaphoreHandle_t s_stopped;
static int s_sock = -1;
static uint32_t s_ip;

/* Change the query in p into the answer. Return the answer length, or 0
 * to send nothing. Only A queries get an address, other types get an
 * empty answer. */
static int answer(uint8_t *p, int len)
{
    int q = HEADER_LEN;
    bool type_a;

    /* A response, not a standard query, or not one question */
    if (len < HEADER_LEN || (p[2] & 0xF8) != 0 || p[4] != 0 || p[5] != 1) {
        return 0;
    }
    while (q < len && p[q] != 0) {
        if (p[q] & 0xC0) {
            return 0;   /* A question has no compressed names. */
        }
        q += p[q] + 1;
    }
    q += 5;             /* End of name, type, class */
    if (q > len || q + ANSWER_LEN > PACKET_MAX) {
        return 0;
    }
    type_a = p[q - 4] == 0 && p[q - 3] == 1 && p[q - 2] == 0 && p[q - 1] == 1;

    p[2] = 0x84 | (p[2] & 0x01);    /* Response, authoritative, keep RD */
    p[3] = 0;                       /* No error */
    p[6] = 0;
    p[7] = type_a;                  /* Answers */
    memset(&p[8], 0, 4);            /* No authority or additional records */
    if (!type_a) {
        return q;
    }
    /* Name: pointer to the question. Type A, class IN, TTL 0, 4 bytes. */
    memcpy(&p[q], "\xC0\x0C\x00\x01\x00\x01\x00\x00\x00\x00\x00\x04", 12);
    memcpy(&p[q + 12], &s_ip, 4);
    return q + ANSWER_LEN;
}

static void dns_task(void *arg)
{
    uint8_t buf[PACKET_MAX];
    struct sockaddr_in from;
    socklen_t from_len;
    int len;

    while (s_run) {
        from_len = sizeof(from);
        len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len > 0 && (len = answer(buf, len)) > 0) {
            sendto(s_sock, buf, len, 0, (struct sockaddr *)&from, from_len);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));     /* Do not spin on an error. */
        }
    }
    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

esp_err_t dns_start(uint32_t ip)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = { .tv_usec = POLL_MS * 1000 };

    if (!s_stopped && !(s_stopped = xSemaphoreCreateBinary())) {
        return ESP_ERR_NO_MEM;
    }
    s_ip = ip;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    s_run = true;
    if (xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL) != pdPASS) {
        s_run = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_stop(void)
{
    if (s_sock < 0) {
        return;
    }
    s_run = false;
    xSemaphoreTake(s_stopped, portMAX_DELAY);
    close(s_sock);
    s_sock = -1;
}
