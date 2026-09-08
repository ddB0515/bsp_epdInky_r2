#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default RTSP control port. */
#define RTSP_SERVER_PORT 8554

/**
 * @brief Start the RTSP server task.
 *
 * Serves one mount point at the sensor's native resolution:
 *
 *   rtsp://<board>:8554/stream    H.264 1280x720
 *
 * Only the native size is offered because the PPA scaler cannot run while the
 * hardware H.264 encoder is open, so a downscaled H.264 stream is not possible
 * on this chip.
 *
 * Starting a session takes the camera from the MJPEG preview, and starting the
 * preview ends the session: there is one capture queue and one encoder.
 */
esp_err_t rtsp_server_start(void);

/** @brief Stop the RTSP server. */
void rtsp_server_stop(void);

/** @brief Whether a client is currently playing, and if so at what size. */
bool rtsp_server_is_playing(uint32_t *width, uint32_t *height);

#ifdef __cplusplus
}
#endif
