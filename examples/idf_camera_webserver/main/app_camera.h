#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A single frame borrowed from the camera - encoded JPEG or raw YUV420. */
typedef struct {
	const uint8_t *data;
	size_t         len;
	uint32_t       width;
	uint32_t       height;
} app_camera_frame_t;

/** @brief What a stream consumer intends to do with the frames. */
typedef enum {
	APP_STREAM_NONE = 0,
	APP_STREAM_MJPEG,   /**< HTTP MJPEG, needs JPEG-encoded frames */
	APP_STREAM_H264,    /**< RTSP, needs raw YUV420 frames         */
} app_stream_kind_t;

/** @brief Where a control lives, since the sensor and the ISP are separate devices. */
typedef enum {
	APP_CTRL_DEV_SENSOR = 0,   /**< /dev/video0  - exposure, gain, flip, ... */
	APP_CTRL_DEV_ISP,          /**< /dev/video20 - colour gains              */
	APP_CTRL_DEV_LOCAL,        /**< handled by this app, not by a driver     */
} app_camera_ctrl_dev_t;

/** @brief Metadata and current value for one tunable control. */
typedef struct {
	uint32_t              id;         /**< V4L2 control id, or a synthetic APP_CID_* */
	const char           *key;        /**< stable machine-readable name              */
	const char           *label;      /**< human-readable name for the UI            */
	app_camera_ctrl_dev_t device;
	int32_t               min;
	int32_t               max;
	int32_t               step;
	int32_t               def;
	int32_t               value;      /**< current value                             */
	bool                  is_bool;    /**< render as a switch rather than a slider   */
} app_camera_ctrl_t;

/* Synthetic controls implemented by this application rather than a driver.
 * They sit above the V4L2 user range so they cannot collide with real ids. */
#define APP_CID_BASE          0x08000000
#define APP_CID_AUTO_EXPOSURE (APP_CID_BASE + 0)
#define APP_CID_JPEG_QUALITY  (APP_CID_BASE + 1)
#define APP_CID_AE_TARGET     (APP_CID_BASE + 2)
#define APP_CID_RED_GAIN      (APP_CID_BASE + 3)
#define APP_CID_BLUE_GAIN     (APP_CID_BASE + 4)

/**
 * @brief Bring up the MIPI CSI sensor and the hardware JPEG encoder.
 *
 * The shared I2C bus from the BSP is reused as the sensor's SCCB bus, so
 * bsp_i2c_init() (or any bsp_*_init) must have run first.
 */
esp_err_t app_camera_init(void);

/** @brief Release the camera and encoder. */
void app_camera_deinit(void);

/**
 * @brief Stop the sensor streaming without dismantling the pipeline.
 *
 * Used when going to sleep. The devices stay open because esp_video cannot be
 * re-initialised once deinitialised - see the comment on the implementation.
 */
esp_err_t app_camera_suspend(void);

/** @brief Restart streaming after app_camera_suspend(). */
esp_err_t app_camera_resume(void);

/**
 * @brief Capture one frame and encode it as JPEG at the requested size.
 *
 * Pass 0,0 for the sensor's native size. Any smaller size is produced by the
 * PPA, which also does the YUV420 to RGB565 conversion the JPEG encoder needs
 * on this silicon revision.
 */
esp_err_t app_camera_capture_jpeg(uint32_t width, uint32_t height, app_camera_frame_t *out);

/**
 * @brief Capture one frame as raw YUV420 at the sensor's native size.
 *
 * The buffer is in the ESP32-P4's native OUYY_EVYY layout, which is what the
 * hardware H.264 encoder expects, so the camera buffer is handed straight out
 * with no copy and no conversion.
 */
esp_err_t app_camera_capture_yuv(app_camera_frame_t *out);

/**
 * @brief Return the buffer obtained from a capture call.
 *
 * Must be called for every successful capture, otherwise the driver runs out
 * of buffers and streaming stalls.
 */
void app_camera_release(void);

/** @brief The sensor's native capture resolution. */
void app_camera_get_size(uint32_t *width, uint32_t *height);

/* ===========================================================================
 * Stream arbitration
 *
 * The camera has one capture queue and there is one H.264 encoder, so only one
 * consumer (the MJPEG preview or one RTSP session) can run at a time.
 * Acquiring hands ownership to the new caller and signals the previous one to
 * stop. Since the sensor mode never changes, a handover is quick.
 * ========================================================================= */

/**
 * @brief Take ownership of the camera, switching sensor mode if asked.
 *
 * Any existing consumer is asked to stop and is waited for. Pass 0,0 to keep
 * whatever mode is current.
 *
 * @return A non-zero ownership token, or 0 if ownership could not be taken.
 */
uint32_t app_camera_stream_acquire(app_stream_kind_t kind, uint32_t width, uint32_t height);

/**
 * @brief Put the sensor into a different mode.
 *
 * Only sizes the sensor natively supports are accepted - see app_sensor.h. The
 * H.264 encoder can only be fed the native size, so this is how the streams
 * change resolution.
 *
 * @return ESP_ERR_NOT_FOUND if the sensor has no such mode.
 */
esp_err_t app_camera_set_mode(uint32_t width, uint32_t height);

/**
 * @brief Whether the given token still owns the camera.
 *
 * Streaming loops should check this each frame and exit when it returns false.
 */
bool app_camera_stream_is_owner(uint32_t token);

/** @brief Give up ownership taken by app_camera_stream_acquire(). */
void app_camera_stream_release(uint32_t token);

/** @brief What is streaming right now, for status reporting. */
app_stream_kind_t app_camera_stream_kind(void);

/* ===========================================================================
 * Runtime controls
 * ========================================================================= */

/**
 * @brief Get the table of available controls.
 *
 * The table is built during init by probing the drivers, so it only lists
 * controls this sensor actually supports.
 *
 * @param out    Receives a pointer to the internal table (do not free).
 * @param count  Receives the number of entries.
 */
void app_camera_get_controls(const app_camera_ctrl_t **out, size_t *count);

/** @brief Set one control by id. Returns ESP_ERR_NOT_FOUND for unknown ids. */
esp_err_t app_camera_set_control(uint32_t id, int32_t value);

/** @brief Read back one control by id. */
esp_err_t app_camera_get_control(uint32_t id, int32_t *value);

/** @brief Restore every control to its default. */
void app_camera_reset_controls(void);

/** @brief Most recent measured scene brightness (0-255), for the UI. */
int32_t app_camera_get_measured_luma(void);

#ifdef __cplusplus
}
#endif
