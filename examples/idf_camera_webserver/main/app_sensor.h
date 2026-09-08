#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One capture mode the sensor can be put into. */
typedef struct {
	uint32_t    width;
	uint32_t    height;
	uint32_t    fps;
	const char *name;    /**< the driver's own name for the mode */
	const void *format;  /**< opaque esp_cam_sensor_format_t*, owned by the driver */
} app_sensor_mode_t;

/**
 * @brief Find the camera sensor and build the list of modes it offers.
 *
 * Must be called after esp_video_init().
 */
esp_err_t app_sensor_probe(void);

/** @brief The modes this sensor supports, in the driver's order. */
void app_sensor_get_modes(const app_sensor_mode_t **modes, size_t *count);

/** @brief Look up a mode by exact size, or NULL if the sensor has no such mode. */
const app_sensor_mode_t *app_sensor_find_mode(uint32_t width, uint32_t height);

/**
 * @brief Switch the sensor to a mode.
 *
 * The caller must have stopped streaming first; the capture format and buffers
 * have to be set up again afterwards, because the frame size changes.
 *
 * @param fd    An open file descriptor for the CSI video device.
 * @param mode  A mode from app_sensor_get_modes().
 */
esp_err_t app_sensor_set_mode(int fd, const app_sensor_mode_t *mode);

#ifdef __cplusplus
}
#endif
