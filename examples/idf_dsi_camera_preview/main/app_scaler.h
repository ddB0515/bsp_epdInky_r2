#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the PPA (Pixel Processing Accelerator) client.
 *
 * The PPA is the ESP32-P4's 2D hardware block for scaling, rotation and colour
 * space conversion. It is what lets the sensor stay in one mode while each
 * consumer gets the size and pixel format it needs.
 */
esp_err_t app_scaler_init(void);

/** @brief Release the PPA client and any scratch buffers. */
void app_scaler_deinit(void);

/**
 * @brief Scale a YUV420 frame and convert it to RGB565.
 *
 * Used by the MJPEG preview, which needs RGB565 because this board's ESP32-P4
 * is silicon revision v1.0 and its JPEG encoder only gained YUV420 input on
 * v3.0. The PPA does the colour conversion, and any downscale, in one pass.
 *
 * @note Must not be called while the hardware H.264 encoder is open: the two
 *       blocks cannot run concurrently and the PPA call would never return.
 *       The stream arbitration in app_camera.c guarantees this.
 *
 * @param src      Source frame in the P4's OUYY_EVYY YUV420 layout.
 * @param src_w    Source width, must be even.
 * @param src_h    Source height, must be even.
 * @param dst_w    Destination width, must be even.
 * @param dst_h    Destination height, must be even.
 * @param dst      Receives a pointer to an internal buffer, valid until the
 *                 next call.
 * @param dst_len  Receives the length of the converted frame.
 */
esp_err_t app_scaler_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                            uint32_t dst_w, uint32_t dst_h,
                            const uint8_t **dst, size_t *dst_len);

/**
 * @brief Scale a YUV420 frame into a rectangle of a caller-owned RGB888 image.
 *
 * Used to put the camera preview straight into the display's frame buffer, so
 * there is no intermediate copy: the PPA reads the camera buffer and writes
 * the scan-out buffer in one pass, converting colour space and resizing at the
 * same time.
 *
 * @note Must not be called while the hardware H.264 encoder is open - the two
 *       blocks cannot run concurrently and the PPA call would never return.
 *
 * @param src      Source frame in the P4's OUYY_EVYY YUV420 layout.
 * @param src_w    Source width, must be even.
 * @param src_h    Source height, must be even.
 * @param dst      Destination RGB888 image.
 * @param dst_size Size of @p dst in bytes.
 * @param dst_w    Full width of the destination image (its stride, in pixels).
 * @param dst_h    Full height of the destination image.
 * @param x        Left edge of the target rectangle, must be even.
 * @param y        Top edge of the target rectangle, must be even.
 * @param w        Width of the target rectangle, must be even.
 * @param h        Height of the target rectangle, must be even.
 */
esp_err_t app_scaler_yuv420_to_rgb888_rect(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                           void *dst, size_t dst_size,
                                           uint32_t dst_w, uint32_t dst_h,
                                           uint32_t x, uint32_t y,
                                           uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif
