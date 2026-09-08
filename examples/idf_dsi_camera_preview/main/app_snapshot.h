#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prepare still capture and check whether the SD card is mounted.
 *
 * Safe to call without a card: app_snapshot_sd_available() then reports false
 * and only the download path stays usable.
 */
esp_err_t app_snapshot_init(void);

/** @brief Whether stills can be written to the SD card. */
bool app_snapshot_sd_available(void);

/**
 * @brief Offer the MJPEG stream's current frame to a waiting request.
 *
 * Called by the streaming loop for every frame. If a still was requested, the
 * frame is copied into an internal buffer and the requester is woken.
 *
 * Only a copy is taken here, deliberately: writing to the SD card from inside
 * the streaming loop would stall the stream for the whole write. The
 * requesting task does the slow work instead.
 */
void app_snapshot_offer_frame(const uint8_t *jpeg, size_t len);

/**
 * @brief Obtain one still image.
 *
 * While the MJPEG stream is running this borrows its next frame, so nothing
 * competes for the camera and the image is exactly what the viewer sees.
 * Otherwise the camera is taken for a single shot.
 *
 * On success the snapshot lock is held until app_snapshot_end() is called, so
 * the returned buffer stays valid. Every successful call must be paired with
 * app_snapshot_end().
 *
 * @param stream_running  True if the MJPEG stream is currently active.
 * @param data            Receives the JPEG data.
 * @param len             Receives its length.
 */
esp_err_t app_snapshot_begin(bool stream_running, const uint8_t **data, size_t *len);

/** @brief Release the buffer and lock taken by app_snapshot_begin(). */
void app_snapshot_end(void);

/**
 * @brief Write an image obtained from app_snapshot_begin() to the SD card.
 *
 * Picks the next free /sdcard/snapNNNN.jpg, writes it, then reads it back to
 * confirm the file is a complete JPEG.
 *
 * @param data       Image data.
 * @param len        Image length.
 * @param path       Receives the path that was written.
 * @param path_size  Size of @p path.
 */
esp_err_t app_snapshot_write_sd(const uint8_t *data, size_t len,
                                char *path, size_t path_size);

#ifdef __cplusplus
}
#endif
