#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the web server on port 80. */
esp_err_t app_httpd_start(void);

/** @brief Whether an MJPEG client is currently connected. */
bool app_httpd_is_streaming(void);

/** @brief Stop the web server. */
void app_httpd_stop(void);

#ifdef __cplusplus
}
#endif
