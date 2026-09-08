#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One encoded access unit, still in Annex-B form (start codes included). */
typedef struct {
	const uint8_t *data;
	size_t         len;
	bool           is_keyframe;
} app_h264_frame_t;

/**
 * @brief Open the hardware H.264 encoder for a given resolution.
 *
 * Wraps the esp_video M2M device (/dev/video11). Input is YUV420 in the P4's
 * native OUYY_EVYY layout, which is what the camera already produces when it
 * is configured for the H.264 path, so frames pass through without conversion.
 *
 * @param width     Frame width, must match the camera.
 * @param height    Frame height, must match the camera.
 * @param bitrate   Target bitrate in bits per second.
 * @param i_period  Frames between I-frames (the GOP length).
 */
esp_err_t app_h264_open(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t i_period);

/** @brief Shut the encoder down and release its buffers. */
void app_h264_close(void);

/** @brief Whether the encoder is currently open. */
bool app_h264_is_open(void);

/**
 * @brief Encode one raw YUV420 frame.
 *
 * @param yuv      Raw frame, in OUYY_EVYY layout.
 * @param yuv_len  Size of the raw frame in bytes.
 * @param out      Receives a pointer into the encoder's output buffer, valid
 *                 until the next call.
 */
esp_err_t app_h264_encode(const uint8_t *yuv, size_t yuv_len, app_h264_frame_t *out);

/**
 * @brief Get the SPS and PPS captured from the stream.
 *
 * These are needed for the SDP that RTSP clients fetch before they can decode
 * anything, so they are cached the first time the encoder emits them.
 *
 * @return ESP_ERR_INVALID_STATE until at least one keyframe has been encoded.
 */
esp_err_t app_h264_get_parameter_sets(const uint8_t **sps, size_t *sps_len,
                                      const uint8_t **pps, size_t *pps_len);

#ifdef __cplusplus
}
#endif
