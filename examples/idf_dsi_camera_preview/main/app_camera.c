/*
 * MIPI CSI camera capture for the epdInky ESP32-P4 board.
 *
 * Uses the official espressif/esp_video component, which exposes the sensor
 * through a Linux-style V4L2 device (/dev/video0).
 *
 * Capture is always YUV420, which is exactly what the H.264 encoder consumes,
 * so the RTSP path does no pixel conversion at all. The MJPEG preview needs
 * RGB565, because this board's ESP32-P4 is silicon revision v1.0 and its JPEG
 * encoder only gained YUV420 input on v3.0, so the PPA converts on the way
 * through (see app_scaler.c).
 *
 * The sensor stays in one mode for the life of the program. Switching between
 * the preview and the RTSP stream therefore costs nothing: no camera restart,
 * and auto-exposure carries on from where it was instead of starting over.
 *
 * The ISP pipeline controller (and with it the vendor auto-exposure and auto
 * white balance) is disabled because of an upstream bug - see the README - so
 * this file implements its own auto-exposure loop and exposes the sensor and
 * ISP controls for manual tuning.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/jpeg_encode.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_isp_ioctl.h"
#include "linux/videodev2.h"

#include "bsp/epdinky_p4_board.h"

#include "app_camera.h"
#include "app_scaler.h"
#include "app_sensor.h"

static const char *TAG = "app_camera";

/* The sensor's native mode, and the size every stream is served at. 720p is
 * chosen over 1080p because it holds a solid 30 fps; 1080p managed only ~15. */
#define APP_CAMERA_WIDTH  1280
#define APP_CAMERA_HEIGHT  720

/* Enough buffers to keep the sensor running while one frame is being encoded. */
#define APP_CAMERA_BUF_COUNT 2

#define APP_MAX_CONTROLS 12

/* Auto exposure tuning. */
#define AE_DEFAULT_TARGET   105   /* aiming a little below mid-grey keeps highlights */
#define AE_TOLERANCE          6   /* dead band, stops the loop hunting               */
#define AE_INTERVAL_FRAMES    3   /* sensor needs a frame or two to react            */
#define AE_MAX_STEP_PERCENT  25   /* damping: cap how far one correction can move    */

struct camera_buffer {
	uint8_t *start;
	size_t   length;
};

static int                    s_fd = -1;
static int                    s_isp_fd = -1;
static struct camera_buffer   s_buffers[APP_CAMERA_BUF_COUNT];
static uint32_t               s_buf_count;
static uint32_t               s_width;
static uint32_t               s_height;

static jpeg_encoder_handle_t  s_jpeg;
static uint8_t               *s_jpeg_buf;
static size_t                 s_jpeg_buf_size;
static uint32_t               s_jpeg_width;
static uint32_t               s_jpeg_height;

/* Index of the buffer currently handed out to the caller, -1 if none. */
static int                    s_queued_index = -1;

/* The stream task and the HTTP control handlers run in different threads, so
 * every ioctl and control-table access is serialised. */
static SemaphoreHandle_t      s_lock;

static app_camera_ctrl_t      s_ctrls[APP_MAX_CONTROLS];
static size_t                 s_ctrl_count;

/* Stream ownership, see the arbitration section further down. */
static uint32_t               s_stream_token;
static bool                   s_stream_busy;
static app_stream_kind_t      s_stream_kind;

static int32_t                s_jpeg_quality  = 80;
static bool                   s_auto_exposure = true;
static int32_t                s_ae_target     = AE_DEFAULT_TARGET;
static int32_t                s_measured_luma;

/* ===========================================================================
 * Small helpers
 * ========================================================================= */

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

static int device_fd(app_camera_ctrl_dev_t dev)
{
	return (dev == APP_CTRL_DEV_ISP) ? s_isp_fd : s_fd;
}

static esp_err_t v4l2_set(int fd, uint32_t id, int32_t value)
{
	struct v4l2_ext_control ctrl = { .id = id, .value = value };
	struct v4l2_ext_controls ctrls = {
		.ctrl_class = V4L2_CTRL_CLASS_USER,
		.count      = 1,
		.controls   = &ctrl,
	};
	return (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) ? ESP_OK : ESP_FAIL;
}

static app_camera_ctrl_t *find_ctrl(uint32_t id)
{
	for (size_t i = 0; i < s_ctrl_count; i++) {
		if (s_ctrls[i].id == id) {
			return &s_ctrls[i];
		}
	}
	return NULL;
}

static int32_t clamp_to(const app_camera_ctrl_t *c, int32_t v)
{
	if (v < c->min) {
		return c->min;
	}
	if (v > c->max) {
		return c->max;
	}
	return v;
}

/* ===========================================================================
 * Control table
 * ========================================================================= */

static void add_local_ctrl(uint32_t id, const char *key, const char *label,
                           int32_t min, int32_t max, int32_t step,
                           int32_t def, int32_t value, bool is_bool)
{
	if (s_ctrl_count >= APP_MAX_CONTROLS) {
		return;
	}
	s_ctrls[s_ctrl_count++] = (app_camera_ctrl_t){
		.id = id, .key = key, .label = label, .device = APP_CTRL_DEV_LOCAL,
		.min = min, .max = max, .step = step, .def = def, .value = value,
		.is_bool = is_bool,
	};
}

/* Probe a driver control; only add it if the driver actually reports it. */
static void probe_driver_ctrl(app_camera_ctrl_dev_t dev, uint32_t id,
                              const char *key, const char *label, bool is_bool)
{
	if (s_ctrl_count >= APP_MAX_CONTROLS) {
		return;
	}

	int fd = device_fd(dev);
	if (fd < 0) {
		return;
	}

	struct v4l2_query_ext_ctrl q = { .id = id };
	if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) != 0) {
		ESP_LOGD(TAG, "control %s not supported", key);
		return;
	}

	int32_t current = (int32_t)q.default_value;
	struct v4l2_ext_control ctrl = { .id = id };
	struct v4l2_ext_controls ctrls = {
		.ctrl_class = V4L2_CTRL_CLASS_USER, .count = 1, .controls = &ctrl,
	};
	/* Some controls are settable but not readable - the SC2336's flips, for
	 * instance - so fall back to the driver's default. */
	bool readable = (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0);
	if (readable) {
		current = ctrl.value;
	}

	s_ctrls[s_ctrl_count++] = (app_camera_ctrl_t){
		.id = id, .key = key, .label = label, .device = dev,
		.min = (int32_t)q.minimum, .max = (int32_t)q.maximum,
		.step = q.step ? (int32_t)q.step : 1,
		.def = (int32_t)q.default_value, .value = current,
		.is_bool = is_bool,
	};

	ESP_LOGI(TAG, "  %-14s range %" PRId32 "..%" PRId32 " step %" PRId32 " (%s %" PRId32 ")",
	         key, (int32_t)q.minimum, (int32_t)q.maximum,
	         q.step ? (int32_t)q.step : 1,
	         readable ? "now" : "default, write-only:", current);
}

static void build_control_table(void)
{
	s_ctrl_count = 0;

	ESP_LOGI(TAG, "Available camera controls:");

	/*
	 * Probing is the whole point here: we ask for controls the sensor may not
	 * have, and skip the ones it rejects. esp_video logs those rejections at
	 * ERROR level, which makes a perfectly healthy boot look broken - on the
	 * SC2336 it complains about ANALOGUE_GAIN (genuinely unsupported) and
	 * about reading back HFLIP/VFLIP (settable, but not readable, so the
	 * default is used instead).
	 *
	 * Mute those two tags for the duration of the probe so real errors are
	 * not lost among the expected ones. The table logged below is the
	 * authoritative list of what this sensor actually supports.
	 */
	esp_log_level_t prev_cam   = esp_log_level_get("esp_video_cam");
	esp_log_level_t prev_video = esp_log_level_get("esp_video");
	esp_log_level_set("esp_video_cam", ESP_LOG_NONE);
	esp_log_level_set("esp_video", ESP_LOG_NONE);

	/* Sensor side. */
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_EXPOSURE,      "exposure",  "Exposure",      false);
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_GAIN,          "gain",      "Gain",          false);
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_ANALOGUE_GAIN, "again",     "Analogue gain", false);
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_HFLIP,         "hflip",     "Mirror",        true);
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_VFLIP,         "vflip",     "Flip",          true);
	probe_driver_ctrl(APP_CTRL_DEV_SENSOR, V4L2_CID_TEST_PATTERN,  "testpat",   "Test pattern",  false);

	esp_log_level_set("esp_video_cam", prev_cam);
	esp_log_level_set("esp_video", prev_video);

	/* Application-side controls. */
	add_local_ctrl(APP_CID_AUTO_EXPOSURE, "auto_exposure", "Auto exposure",
	               0, 1, 1, 1, s_auto_exposure ? 1 : 0, true);
	add_local_ctrl(APP_CID_AE_TARGET, "ae_target", "Brightness target",
	               40, 200, 1, AE_DEFAULT_TARGET, s_ae_target, false);
	add_local_ctrl(APP_CID_JPEG_QUALITY, "jpeg_quality", "JPEG quality",
	               10, 100, 1, 80, s_jpeg_quality, false);

	/* Colour gains live on the ISP; 1000 == 1.00x. */
	if (s_isp_fd >= 0) {
		add_local_ctrl(APP_CID_RED_GAIN,  "red_gain",  "Red gain",  1000, 4000, 10, 1900, 1900, false);
		add_local_ctrl(APP_CID_BLUE_GAIN, "blue_gain", "Blue gain", 1000, 4000, 10, 1750, 1750, false);
	}
}

/* ===========================================================================
 * Auto exposure
 * ========================================================================= */

/*
 * Estimate scene brightness from the captured frame.
 *
 * The P4's YUV420 is the OUYY_EVYY layout, which is interleaved rather than
 * planar: bytes run as chroma, luma, luma repeating. Stepping in whole triplets
 * keeps the phase, so we only ever read luma - no unpacking or colour
 * weighting needed.
 *
 * Sampling a sparse grid rather than every pixel keeps this to a fraction of a
 * millisecond, which matters because it runs in the capture path.
 * Metering deliberately uses the full-resolution frame, before any scaling, so
 * exposure behaves identically whichever consumer is active.
 */
static int32_t measure_luma(const uint8_t *buf, size_t len)
{
	size_t groups = len / 3;
	if (groups == 0) {
		return 0;
	}

	/* Two luma samples per triplet, so 2000 triplets gives ~4000 samples. */
	size_t stride = groups / 2000;
	if (stride < 1) {
		stride = 1;
	}

	uint32_t sum = 0, count = 0;
	for (size_t g = 0; g < groups; g += stride) {
		const uint8_t *triplet = &buf[g * 3];
		sum += triplet[1];
		sum += triplet[2];
		count += 2;
	}
	return count ? (int32_t)(sum / count) : 0;
}

/*
 * One step of the exposure feedback loop.
 *
 * Exposure is adjusted first because it costs no noise; gain is only brought in
 * once exposure has hit the end of its range. Each correction is damped and
 * capped so the image settles instead of oscillating.
 */
static void auto_exposure_step(int32_t luma)
{
	app_camera_ctrl_t *exp  = find_ctrl(V4L2_CID_EXPOSURE);
	app_camera_ctrl_t *gain = find_ctrl(V4L2_CID_GAIN);
	if (!exp) {
		return;
	}

	int32_t error = s_ae_target - luma;
	if (error > -AE_TOLERANCE && error < AE_TOLERANCE) {
		return;
	}

	/* Scale the correction by how far off we are, as a fraction of target. */
	int32_t pct = (error * 100) / (s_ae_target ? s_ae_target : 1);
	if (pct >  AE_MAX_STEP_PERCENT) pct =  AE_MAX_STEP_PERCENT;
	if (pct < -AE_MAX_STEP_PERCENT) pct = -AE_MAX_STEP_PERCENT;

	int32_t new_exp = exp->value + (exp->value * pct) / 100;
	/* Guarantee progress when the proportional term rounds to zero. */
	if (new_exp == exp->value) {
		new_exp += (error > 0) ? exp->step : -exp->step;
	}
	new_exp = clamp_to(exp, new_exp);

	if (new_exp != exp->value) {
		if (v4l2_set(s_fd, exp->id, new_exp) == ESP_OK) {
			exp->value = new_exp;
		}
		return;
	}

	/* Exposure is pinned at an end stop, so trade noise for brightness. */
	if (gain) {
		int32_t new_gain = gain->value + (error > 0 ? gain->step : -gain->step);
		new_gain = clamp_to(gain, new_gain);
		if (new_gain != gain->value && v4l2_set(s_fd, gain->id, new_gain) == ESP_OK) {
			gain->value = new_gain;
		}
	}
}

/* ===========================================================================
 * Sensor bring-up
 * ========================================================================= */

static esp_err_t camera_video_init(void)
{
	/* Reuse the BSP's I2C bus as the sensor's SCCB bus rather than letting
	 * esp_video create a second master on the same pins. */
	i2c_master_bus_handle_t i2c_bus = NULL;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&i2c_bus), TAG, "I2C bus not available");

	esp_video_init_csi_config_t csi_config = {
		.sccb_config = {
			.init_sccb  = false,
			.i2c_handle = i2c_bus,
			.freq       = BSP_I2C_FREQ_HZ,
		},
		.reset_pin     = BSP_CSI_PIN_RESET,
		.pwdn_pin      = BSP_CSI_PIN_PWDN,
		.dont_init_ldo = false,
	};

	const esp_video_init_config_t video_config = {
		.csi = &csi_config,
	};

	ESP_RETURN_ON_ERROR(esp_video_init(&video_config), TAG,
	                    "esp_video_init failed - is the camera connected?");
	return ESP_OK;
}

/*
 * Ask for YUV420 at whatever size the sensor is currently producing.
 *
 * The CSI driver takes the frame size from the active sensor mode, but it does
 * validate the width and height handed to VIDIOC_S_FMT against that mode and
 * rejects a mismatch. So the current size is read back first rather than
 * assumed - which is also what makes this work unchanged after a mode switch.
 */
static esp_err_t camera_configure(void)
{
	struct v4l2_format fmt = {0};
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_G_FMT failed");

	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_S_FMT, &fmt) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_S_FMT for YUV420 at %" PRIu32 "x%" PRIu32 " failed",
	                    (uint32_t)fmt.fmt.pix.width, (uint32_t)fmt.fmt.pix.height);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_G_FMT failed");

	ESP_RETURN_ON_FALSE(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420,
	                    ESP_ERR_NOT_SUPPORTED, TAG,
	                    "the sensor pipeline refused YUV420");

	s_width  = fmt.fmt.pix.width;
	s_height = fmt.fmt.pix.height;

	ESP_LOGI(TAG, "Capturing %" PRIu32 "x%" PRIu32 " YUV420", s_width, s_height);
	return ESP_OK;
}

static esp_err_t camera_open_device(void)
{
	s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
	ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG,
	                    "failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

	struct v4l2_capability cap;
	if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0) {
		ESP_LOGI(TAG, "Video device: %s", (const char *)cap.card);
	}

	ESP_RETURN_ON_ERROR(camera_configure(), TAG, "configuring the capture format failed");

	/* Optional: colour gains live here. Not fatal if it is unavailable. */
	s_isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
	if (s_isp_fd < 0) {
		ESP_LOGW(TAG, "ISP device unavailable, colour gains cannot be adjusted");
	}
	return ESP_OK;
}

static esp_err_t camera_start_streaming(void)
{
	struct v4l2_requestbuffers req = {
		.count  = APP_CAMERA_BUF_COUNT,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_REQBUFS failed");
	s_buf_count = req.count;

	for (uint32_t i = 0; i < s_buf_count; i++) {
		struct v4l2_buffer buf = {
			.index  = i,
			.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.memory = V4L2_MEMORY_MMAP,
		};
		ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG,
		                    "VIDIOC_QUERYBUF failed for buffer %" PRIu32, i);

		s_buffers[i].length = buf.length;
		s_buffers[i].start  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
		                           MAP_SHARED, s_fd, buf.m.offset);
		ESP_RETURN_ON_FALSE(s_buffers[i].start != MAP_FAILED, ESP_ERR_NO_MEM, TAG,
		                    "mmap failed for buffer %" PRIu32, i);

		ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG,
		                    "VIDIOC_QBUF failed for buffer %" PRIu32, i);
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_STREAMON failed");
	return ESP_OK;
}

/*
 * Tear the capture side down far enough that the format can be changed.
 *
 * The buffers are sized for the current resolution, so they have to be
 * unmapped and handed back (REQBUFS count=0) before a new format is set.
 */
static void camera_stop_streaming(void)
{
	if (s_fd < 0) {
		return;
	}

	/* Any frame still checked out would dangle once the buffers go away. */
	if (s_queued_index >= 0) {
		s_queued_index = -1;
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(s_fd, VIDIOC_STREAMOFF, &type);

	for (uint32_t i = 0; i < s_buf_count; i++) {
		if (s_buffers[i].start) {
			munmap(s_buffers[i].start, s_buffers[i].length);
			s_buffers[i].start  = NULL;
			s_buffers[i].length = 0;
		}
	}
	s_buf_count = 0;

	struct v4l2_requestbuffers req = {
		.count  = 0,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ioctl(s_fd, VIDIOC_REQBUFS, &req);
}

/* ===========================================================================
 * Hardware JPEG encoder
 * ========================================================================= */

static esp_err_t camera_jpeg_alloc_buffer(uint32_t width, uint32_t height)
{
	if (s_jpeg_buf) {
		free(s_jpeg_buf);
		s_jpeg_buf      = NULL;
		s_jpeg_buf_size = 0;
	}

	/* The encoder needs DMA-capable, specially aligned memory (it comes from
	 * PSRAM, so being generous is cheap).
	 *
	 * Size it for the worst case rather than the typical one: at quality 100 a
	 * detailed or noisy frame compresses very little, and the encoder fails
	 * the whole frame if its output does not fit ("the generated image is
	 * larger than the buffer provided by the user"). The RGB565 source is two
	 * bytes per pixel, so a JPEG bigger than that is essentially impossible;
	 * the extra 64 KB covers headers and alignment. */
	jpeg_encode_memory_alloc_cfg_t mem_cfg = {
		.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
	};
	size_t requested = (size_t)width * height * 2 + (64 * 1024);
	s_jpeg_buf = jpeg_alloc_encoder_mem(requested, &mem_cfg, &s_jpeg_buf_size);
	ESP_RETURN_ON_FALSE(s_jpeg_buf, ESP_ERR_NO_MEM, TAG, "no memory for the JPEG buffer");

	s_jpeg_width  = width;
	s_jpeg_height = height;
	return ESP_OK;
}

static esp_err_t camera_jpeg_init(void)
{
	jpeg_encode_engine_cfg_t engine_cfg = {
		.timeout_ms = 5000,
	};
	ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine_cfg, &s_jpeg), TAG,
	                    "failed to create the JPEG encoder");

	ESP_RETURN_ON_ERROR(camera_jpeg_alloc_buffer(s_width, s_height), TAG,
	                    "JPEG buffer alloc failed");

	ESP_LOGI(TAG, "Hardware JPEG encoder ready (%u byte output buffer)",
	         (unsigned)s_jpeg_buf_size);
	return ESP_OK;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t app_camera_init(void)
{
	/* Coming back from a nap: the pipeline was only suspended, not destroyed,
	 * because esp_video cannot be re-initialised (see app_camera_suspend). */
	if (s_fd >= 0) {
		return app_camera_resume();
	}

	s_lock = xSemaphoreCreateMutex();
	ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "failed to create the camera mutex");

	ESP_RETURN_ON_ERROR(camera_video_init(), TAG, "video init failed");
	ESP_RETURN_ON_ERROR(camera_open_device(), TAG, "opening the video device failed");
	ESP_RETURN_ON_ERROR(app_sensor_probe(), TAG, "sensor mode discovery failed");
	ESP_RETURN_ON_ERROR(app_scaler_init(), TAG, "PPA scaler init failed");

	build_control_table();

	/* Apply the colour gains once up front. A Bayer sensor has twice as many
	 * green photosites as red or blue, so without this the image is green. */
	app_camera_set_control(APP_CID_RED_GAIN, 1900);
	app_camera_set_control(APP_CID_BLUE_GAIN, 1750);

	ESP_RETURN_ON_ERROR(camera_start_streaming(), TAG, "starting the stream failed");
	ESP_RETURN_ON_ERROR(camera_jpeg_init(), TAG, "JPEG encoder init failed");

	ESP_LOGI(TAG, "Auto exposure %s (target luma %" PRId32 ")",
	         s_auto_exposure ? "on" : "off", s_ae_target);
	return ESP_OK;
}

void app_camera_deinit(void)
{
	if (s_fd >= 0) {
		camera_stop_streaming();
		close(s_fd);
		s_fd = -1;
	}
	if (s_isp_fd >= 0) {
		close(s_isp_fd);
		s_isp_fd = -1;
	}
	if (s_jpeg_buf) {
		free(s_jpeg_buf);
		s_jpeg_buf      = NULL;
		s_jpeg_buf_size = 0;
		s_jpeg_width    = 0;
		s_jpeg_height   = 0;
	}
	if (s_jpeg) {
		jpeg_del_encoder_engine(s_jpeg);
		s_jpeg = NULL;
	}
	app_scaler_deinit();

	/* The stream token keeps counting up, so any consumer still holding an
	 * old one correctly sees that it no longer owns the camera. */
	s_stream_busy = false;
	s_stream_kind = APP_STREAM_NONE;
	s_queued_index = -1;

	if (s_lock) {
		vSemaphoreDelete(s_lock);
		s_lock = NULL;
	}
}

/*
 * Dequeue one frame and run the metering/exposure loop on it.
 *
 * Both the JPEG and the raw paths go through here, so auto-exposure keeps
 * working identically whether the frame ends up in the MJPEG stream or in the
 * H.264 encoder.
 */
static esp_err_t camera_dequeue(uint32_t *index, uint32_t *bytesused)
{
	ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_ERR_INVALID_STATE, TAG, "camera is not running");
	/* Releasing the previous frame first keeps the buffer accounting simple. */
	ESP_RETURN_ON_FALSE(s_queued_index < 0, ESP_ERR_INVALID_STATE, TAG,
	                    "previous frame was not released");

	struct v4l2_buffer buf = {
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_DQBUF, &buf) == 0, ESP_FAIL, TAG,
	                    "VIDIOC_DQBUF failed");
	s_queued_index = buf.index;

	lock();
	static uint32_t frame_counter;
	s_measured_luma = measure_luma(s_buffers[buf.index].start, buf.bytesused);
	if (s_auto_exposure && (++frame_counter % AE_INTERVAL_FRAMES == 0)) {
		auto_exposure_step(s_measured_luma);
	}
	unlock();

	*index     = buf.index;
	*bytesused = buf.bytesused;
	return ESP_OK;
}

/*
 * Stop the sensor without dismantling the pipeline.
 *
 * esp_video_deinit() cannot be undone: it leaves the ISP video device
 * registered, so the next esp_video_init() fails with
 *   "Failed to register video VFS dev name=video20"
 *   "Failed to create hardware ISP video device"
 * Verified by tearing the camera down and bringing it straight back with the
 * console attached.
 *
 * Suspending instead is also the better behaviour: VIDIOC_STREAMOFF stops the
 * sensor streaming and frees the frame buffers, which is where the power goes,
 * and coming back is a couple of ioctls rather than a full re-detection.
 */
esp_err_t app_camera_suspend(void)
{
	if (s_fd < 0) {
		return ESP_OK;
	}
	lock();
	camera_stop_streaming();
	unlock();
	ESP_LOGI(TAG, "Camera suspended");
	return ESP_OK;
}

esp_err_t app_camera_resume(void)
{
	ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_ERR_INVALID_STATE, TAG, "camera was never started");

	lock();
	esp_err_t err = camera_start_streaming();
	unlock();

	if (err == ESP_OK) {
		ESP_LOGI(TAG, "Camera resumed at %" PRIu32 "x%" PRIu32, s_width, s_height);
	}
	return err;
}

esp_err_t app_camera_capture_yuv(app_camera_frame_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	uint32_t index, bytesused;
	ESP_RETURN_ON_ERROR(camera_dequeue(&index, &bytesused), TAG, "capture failed");

	out->data   = s_buffers[index].start;
	out->len    = bytesused;
	out->width  = s_width;
	out->height = s_height;
	return ESP_OK;
}

esp_err_t app_camera_capture_jpeg(uint32_t width, uint32_t height, app_camera_frame_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	if (width == 0 || height == 0) {
		width  = s_width;
		height = s_height;
	}

	uint32_t index, bytesused;
	ESP_RETURN_ON_ERROR(camera_dequeue(&index, &bytesused), TAG, "capture failed");

	/* The JPEG encoder on this silicon cannot take YUV420, so the PPA does the
	 * colour conversion (and any downscale) in one pass. */
	const uint8_t *rgb;
	size_t         rgb_len;
	esp_err_t err = app_scaler_rgb565(s_buffers[index].start, s_width, s_height,
	                                  width, height, &rgb, &rgb_len);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "RGB565 conversion failed: %s", esp_err_to_name(err));
		app_camera_release();
		return err;
	}

	lock();
	if (s_jpeg_width != width || s_jpeg_height != height) {
		esp_err_t alloc_err = camera_jpeg_alloc_buffer(width, height);
		if (alloc_err != ESP_OK) {
			unlock();
			app_camera_release();
			return alloc_err;
		}
	}

	jpeg_encode_cfg_t encode_cfg = {
		.src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
		.sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
		.image_quality = s_jpeg_quality,
		.width         = width,
		.height        = height,
	};

	uint32_t jpeg_len = 0;
	err = jpeg_encoder_process(s_jpeg, &encode_cfg, rgb, rgb_len,
	                           s_jpeg_buf, s_jpeg_buf_size, &jpeg_len);
	unlock();

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
		app_camera_release();
		return err;
	}

	out->data   = s_jpeg_buf;
	out->len    = jpeg_len;
	out->width  = width;
	out->height = height;
	return ESP_OK;
}

void app_camera_release(void)
{
	if (s_queued_index < 0) {
		return;
	}

	struct v4l2_buffer buf = {
		.index  = (uint32_t)s_queued_index,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
		ESP_LOGW(TAG, "VIDIOC_QBUF failed while releasing buffer %d", s_queued_index);
	}
	s_queued_index = -1;
}

void app_camera_get_size(uint32_t *width, uint32_t *height)
{
	if (width) {
		*width = s_width;
	}
	if (height) {
		*height = s_height;
	}
}

/* ===========================================================================
 * Stream arbitration
 * ========================================================================= */

/*
 * Switch the sensor to a different mode.
 *
 * Everything downstream depends on the frame size, so the capture has to be
 * torn down and rebuilt: buffers are sized for the old resolution, and the
 * control ranges change too (exposure is bounded by the mode's frame length,
 * so its maximum differs per mode).
 *
 * The caller must hold the lock and must have no frame checked out.
 */
static esp_err_t camera_switch_mode(const app_sensor_mode_t *mode)
{
	if (mode->width == s_width && mode->height == s_height) {
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Switching sensor to %" PRIu32 "x%" PRIu32, mode->width, mode->height);

	camera_stop_streaming();

	esp_err_t err = app_sensor_set_mode(s_fd, mode);
	if (err == ESP_OK) {
		err = camera_configure();
	}
	if (err == ESP_OK) {
		err = camera_start_streaming();
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
		return err;
	}

	/* Exposure limits are mode-specific, so the table has to be rebuilt. The
	 * application-side controls keep their values; only the driver-backed
	 * entries are re-probed. */
	build_control_table();
	return ESP_OK;
}

esp_err_t app_camera_set_mode(uint32_t width, uint32_t height)
{
	const app_sensor_mode_t *mode = app_sensor_find_mode(width, height);
	ESP_RETURN_ON_FALSE(mode, ESP_ERR_NOT_FOUND, TAG,
	                    "the sensor has no %" PRIu32 "x%" PRIu32 " mode", width, height);

	lock();
	esp_err_t err = camera_switch_mode(mode);
	unlock();
	return err;
}

/*
 * Ownership is a monotonically increasing token. Taking the camera bumps the
 * token, which is how the previous owner discovers it has been displaced: its
 * own token no longer matches, so its next app_camera_stream_is_owner() call
 * returns false and its loop exits.
 */
uint32_t app_camera_stream_acquire(app_stream_kind_t kind, uint32_t width, uint32_t height)
{
	lock();
	uint32_t mine = ++s_stream_token;
	unlock();

	/* Wait for the displaced owner to finish its current frame and let go.
	 * Two seconds is far longer than a frame ever takes; giving up rather
	 * than blocking forever keeps a wedged consumer from deadlocking us. */
	const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
	while (s_stream_busy && xTaskGetTickCount() < deadline) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	lock();
	if (s_stream_token != mine) {
		/* Someone else called acquire while we were waiting; they win. */
		unlock();
		return 0;
	}
	if (s_stream_busy) {
		unlock();
		ESP_LOGW(TAG, "previous stream did not stop in time");
		return 0;
	}

	/* Now that nobody is capturing, the sensor mode can safely change. */
	if (width && height) {
		const app_sensor_mode_t *mode = app_sensor_find_mode(width, height);
		if (!mode) {
			unlock();
			ESP_LOGW(TAG, "no sensor mode for %" PRIu32 "x%" PRIu32, width, height);
			return 0;
		}
		if (camera_switch_mode(mode) != ESP_OK) {
			unlock();
			return 0;
		}
	}

	s_stream_busy = true;
	s_stream_kind = kind;
	unlock();
	return mine;
}

bool app_camera_stream_is_owner(uint32_t token)
{
	return token != 0 && s_stream_token == token;
}

void app_camera_stream_release(uint32_t token)
{
	if (token == 0) {
		return;
	}
	lock();
	/* Only the current owner clears the flag; a displaced owner releasing
	 * late must not stomp on whoever took over. */
	if (s_stream_token == token) {
		s_stream_kind = APP_STREAM_NONE;
	}
	s_stream_busy = false;
	unlock();
}

app_stream_kind_t app_camera_stream_kind(void)
{
	return s_stream_busy ? s_stream_kind : APP_STREAM_NONE;
}

/* ===========================================================================
 * Runtime controls
 * ========================================================================= */

void app_camera_get_controls(const app_camera_ctrl_t **out, size_t *count)
{
	if (out) {
		*out = s_ctrls;
	}
	if (count) {
		*count = s_ctrl_count;
	}
}

esp_err_t app_camera_set_control(uint32_t id, int32_t value)
{
	app_camera_ctrl_t *c = find_ctrl(id);
	if (!c) {
		return ESP_ERR_NOT_FOUND;
	}

	lock();
	value = clamp_to(c, value);
	esp_err_t err = ESP_OK;

	switch (id) {
	case APP_CID_AUTO_EXPOSURE:
		s_auto_exposure = (value != 0);
		break;

	case APP_CID_AE_TARGET:
		s_ae_target = value;
		break;

	case APP_CID_JPEG_QUALITY:
		s_jpeg_quality = value;
		break;

	case APP_CID_RED_GAIN:
	case APP_CID_BLUE_GAIN:
		if (s_isp_fd < 0) {
			err = ESP_ERR_INVALID_STATE;
		} else {
			err = v4l2_set(s_isp_fd,
			               (id == APP_CID_RED_GAIN) ? V4L2_CID_RED_BALANCE
			                                        : V4L2_CID_BLUE_BALANCE,
			               value);
		}
		break;

	default:
		/* Manual exposure and gain only stick while AE is off, otherwise the
		 * loop would immediately overwrite them. */
		if (s_auto_exposure && (id == V4L2_CID_EXPOSURE || id == V4L2_CID_GAIN)) {
			err = ESP_ERR_INVALID_STATE;
		} else {
			err = v4l2_set(device_fd(c->device), id, value);
		}
		break;
	}

	if (err == ESP_OK) {
		c->value = value;
	}
	unlock();
	return err;
}

esp_err_t app_camera_get_control(uint32_t id, int32_t *value)
{
	app_camera_ctrl_t *c = find_ctrl(id);
	if (!c || !value) {
		return ESP_ERR_NOT_FOUND;
	}
	lock();
	*value = c->value;
	unlock();
	return ESP_OK;
}

void app_camera_reset_controls(void)
{
	for (size_t i = 0; i < s_ctrl_count; i++) {
		app_camera_set_control(s_ctrls[i].id, s_ctrls[i].def);
	}
}

int32_t app_camera_get_measured_luma(void)
{
	return s_measured_luma;
}
