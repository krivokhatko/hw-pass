#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Start the DNS server. It answers each A query with ip (network byte order). */
esp_err_t dns_start(uint32_t ip);

/* Stop the DNS server. Safe to call when it is stopped. */
void dns_stop(void);
