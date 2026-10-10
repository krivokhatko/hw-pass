#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the command line task on the USB serial port. */
esp_err_t cli_start(void);

#ifdef __cplusplus
}
#endif
