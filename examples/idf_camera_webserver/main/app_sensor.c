/*
 * Sensor mode discovery and switching.
 *
 * WHY THIS REACHES INTO esp_video's PRIVATE HEADERS
 *
 * The streams need several resolutions, and on this chip only the sensor can
 * provide them: the H.264 encoder must be fed the sensor's native size,
 * because the PPA scaler cannot run while the encoder is open (see
 * app_scaler.c). So the sensor mode has to change at runtime.
 *
 * esp_video offers no supported way to do that:
 *
 *   - VIDIOC_S_FMT ignores the width and height entirely; the CSI driver takes
 *     the size from whichever sensor mode is active.
 *   - VIDIOC_ENUM_FRAMESIZES only ever reports the current size.
 *   - VIDIOC_S_SENSOR_FMT does exist, but it needs an esp_cam_sensor_format_t
 *     that stays valid forever (the driver stores the pointer), and the real
 *     ones live in a static array inside sc2336.c.
 *
 * The official way round this is the video_custom_format example: define your
 * own format descriptors, register tables and all. That would mean copying
 * roughly 700 lines of vendor register data for four modes, which then rots
 * silently when the component is updated.
 *
 * Instead, this asks the driver for its own table. esp_cam_sensor_query_format()
 * hands back pointers straight into that static array - exactly the pointers
 * VIDIOC_S_SENSOR_FMT wants - but it needs the sensor handle, which is only
 * reachable through esp_video's private headers.
 *
 * The private surface is deliberately tiny and confined to this file:
 * esp_video_device_get_object() to find the CSI device, and cam.sensor to get
 * the handle. Including the real headers rather than hardcoding struct offsets
 * means a component update that changes the layout breaks the build instead of
 * corrupting memory.
 */

#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>

#include "esp_check.h"
#include "esp_log.h"

#include "esp_cam_sensor.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"

/* Private headers - see the note at the top of this file. */
#include "esp_video.h"
#include "esp_video_device_common.h"
#include "esp_video_device_internal.h"   /* CSI_NAME */

#include "app_sensor.h"

static const char *TAG = "app_sensor";

#define APP_SENSOR_MAX_MODES 16

static app_sensor_mode_t s_modes[APP_SENSOR_MAX_MODES];
static size_t            s_mode_count;

esp_err_t app_sensor_probe(void)
{
	s_mode_count = 0;

	/* Devices are registered under their driver name ("MIPI-CSI"), not the
	 * /dev/videoN path the V4L2 layer exposes. */
	struct esp_video *video = esp_video_device_get_object(CSI_NAME);
	ESP_RETURN_ON_FALSE(video, ESP_ERR_NOT_FOUND, TAG,
	                    "could not find the %s device object", CSI_NAME);

	esp_cam_sensor_device_t *sensor = VIDEO_DEVICE_COMMON(video)->cam.sensor;
	ESP_RETURN_ON_FALSE(sensor, ESP_ERR_NOT_FOUND, TAG, "no sensor attached");

	esp_cam_sensor_format_array_t array = {0};
	ESP_RETURN_ON_ERROR(esp_cam_sensor_query_format(sensor, &array), TAG,
	                    "could not query the sensor's formats");

	ESP_LOGI(TAG, "Sensor modes (%s):", esp_cam_sensor_get_name(sensor));

	for (uint32_t i = 0; i < array.count && s_mode_count < APP_SENSOR_MAX_MODES; i++) {
		const esp_cam_sensor_format_t *fmt = &array.format_array[i];

		/* The driver lists several variants of the same size (different bit
		 * depths and frame rates), and the streams only care about the
		 * resolution, so keep the first of each and skip the rest. */
		if (app_sensor_find_mode(fmt->width, fmt->height)) {
			continue;
		}

		s_modes[s_mode_count++] = (app_sensor_mode_t){
			.width  = fmt->width,
			.height = fmt->height,
			.fps    = fmt->fps,
			.name   = fmt->name,
			.format = fmt,
		};

		ESP_LOGI(TAG, "  %" PRIu32 "x%" PRIu32 " @ %" PRIu32 " fps",
		         (uint32_t)fmt->width, (uint32_t)fmt->height, (uint32_t)fmt->fps);
	}

	ESP_RETURN_ON_FALSE(s_mode_count, ESP_ERR_NOT_SUPPORTED, TAG,
	                    "the sensor reported no usable modes");
	return ESP_OK;
}

void app_sensor_get_modes(const app_sensor_mode_t **modes, size_t *count)
{
	if (modes) {
		*modes = s_modes;
	}
	if (count) {
		*count = s_mode_count;
	}
}

const app_sensor_mode_t *app_sensor_find_mode(uint32_t width, uint32_t height)
{
	for (size_t i = 0; i < s_mode_count; i++) {
		if (s_modes[i].width == width && s_modes[i].height == height) {
			return &s_modes[i];
		}
	}
	return NULL;
}

esp_err_t app_sensor_set_mode(int fd, const app_sensor_mode_t *mode)
{
	ESP_RETURN_ON_FALSE(mode && mode->format, ESP_ERR_INVALID_ARG, TAG, "invalid mode");
	ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_INVALID_ARG, TAG, "invalid fd");

	/* The driver keeps this pointer, which is why it has to be one of its own
	 * table entries rather than a copy on our stack. */
	if (ioctl(fd, VIDIOC_S_SENSOR_FMT, (void *)mode->format) != 0) {
		ESP_LOGE(TAG, "failed to switch the sensor to %" PRIu32 "x%" PRIu32,
		         mode->width, mode->height);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Sensor now in %" PRIu32 "x%" PRIu32 " @ %" PRIu32 " fps",
	         mode->width, mode->height, mode->fps);
	return ESP_OK;
}
