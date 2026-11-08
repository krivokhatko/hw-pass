#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the command line task on the serial port (components/serial). */
esp_err_t cli_start(void);

#ifdef __cplusplus
}
#endif
