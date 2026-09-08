/*
 * Hardware H.264 encoding for the epdInky ESP32-P4 camera example.
 *
 * The ESP32-P4 has a dedicated H.264 encoder, which esp_video exposes as a
 * memory-to-memory V4L2 device (/dev/video11): you queue a raw frame on the
 * OUTPUT queue and dequeue a compressed one from the CAPTURE queue.
 *
 * The input queue uses USERPTR so the camera's own mmap'd buffer is handed
 * straight to the encoder with no copy, saving over a megabyte of memory
 * traffic per frame at 720p. The output queue uses MMAP and lets the driver
 * size the compressed buffer for us.
 *
 * Why this matters for RTSP: MJPEG needs roughly an order of magnitude more
 * bandwidth than H.264 for the same picture, and the Wi-Fi radio here is
 * attached over SDIO to a separate ESP32-C6, so bandwidth is the scarce
 * resource.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"

#include "esp_video_device.h"
#include "linux/videodev2.h"

#include "app_h264.h"

static const char *TAG = "app_h264";

/* Parameter sets are tiny; these ceilings are far above any real SPS/PPS. */
#define APP_H264_MAX_SPS 64
#define APP_H264_MAX_PPS 32

/*
 * QP window for the rate controller.
 *
 * This must be set explicitly. The esp_video H.264 device defaults to
 * min_qp = 25, max_qp = 26 - a one-step window, which leaves the rate
 * controller no room to move and makes the bitrate target meaningless. With
 * the defaults the encoder simply runs at fixed QP ~25 and produces whatever
 * bitrate that happens to cost (measured: ~9.7 Mbit/s at 720p30, against a
 * 2.5 Mbit/s target).
 *
 * A wider window lets it actually track the target: QP falls for easy frames
 * and rises for busy ones. The bounds keep quality sane at both ends.
 */
#define APP_H264_MIN_QP 18
#define APP_H264_MAX_QP 40

static int      s_fd = -1;
static uint8_t *s_cap_buf;
static size_t   s_cap_len;
static bool     s_streaming;

static uint8_t  s_sps[APP_H264_MAX_SPS];
static size_t   s_sps_len;
static uint8_t  s_pps[APP_H264_MAX_PPS];
static size_t   s_pps_len;

/* ===========================================================================
 * Helpers
 * ========================================================================= */

static esp_err_t set_codec_ctrl(uint32_t id, int32_t value)
{
	struct v4l2_ext_control ctrl = { .id = id, .value = value };
	struct v4l2_ext_controls ctrls = {
		.ctrl_class = V4L2_CID_CODEC_CLASS,
		.count      = 1,
		.controls   = &ctrl,
	};
	return (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t set_queue_format(uint32_t type, uint32_t width, uint32_t height,
                                  uint32_t pixelformat)
{
	struct v4l2_format fmt = {
		.type = type,
		.fmt.pix = {
			.width       = width,
			.height      = height,
			.pixelformat = pixelformat,
		},
	};
	return (ioctl(s_fd, VIDIOC_S_FMT, &fmt) == 0) ? ESP_OK : ESP_FAIL;
}

/*
 * Walk the Annex-B stream and cache the first SPS and PPS we see.
 *
 * RTSP clients need these in the SDP before the first packet arrives, and the
 * encoder only emits them alongside keyframes, so they have to be remembered.
 */
static void capture_parameter_sets(const uint8_t *buf, size_t len)
{
	if (s_sps_len && s_pps_len) {
		return;
	}

	size_t i = 0;
	while (i + 4 <= len) {
		size_t sc = 0;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
			sc = 3;
		} else if (i + 5 <= len && buf[i] == 0 && buf[i + 1] == 0 &&
		           buf[i + 2] == 0 && buf[i + 3] == 1) {
			sc = 4;
		} else {
			i++;
			continue;
		}

		size_t nal_start = i + sc;
		if (nal_start >= len) {
			break;
		}

		/* Find where this NAL ends: the next start code, or the buffer end. */
		size_t nal_end = len;
		for (size_t j = nal_start; j + 3 <= len; j++) {
			if (buf[j] == 0 && buf[j + 1] == 0 &&
			    (buf[j + 2] == 1 || (j + 4 <= len && buf[j + 2] == 0 && buf[j + 3] == 1))) {
				nal_end = j;
				break;
			}
		}

		uint8_t nal_type = buf[nal_start] & 0x1F;
		size_t  nal_len  = nal_end - nal_start;

		if (nal_type == 7 && !s_sps_len && nal_len <= APP_H264_MAX_SPS) {
			memcpy(s_sps, &buf[nal_start], nal_len);
			s_sps_len = nal_len;
			ESP_LOGI(TAG, "cached SPS (%u bytes)", (unsigned)nal_len);
		} else if (nal_type == 8 && !s_pps_len && nal_len <= APP_H264_MAX_PPS) {
			memcpy(s_pps, &buf[nal_start], nal_len);
			s_pps_len = nal_len;
			ESP_LOGI(TAG, "cached PPS (%u bytes)", (unsigned)nal_len);
		}

		i = nal_end;
	}
}

/* True if the access unit contains an IDR slice (NAL type 5). */
static bool contains_keyframe(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i + 4 <= len; i++) {
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
			if ((buf[i + 3] & 0x1F) == 5) {
				return true;
			}
		}
	}
	return false;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t app_h264_open(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t i_period)
{
	ESP_RETURN_ON_FALSE(s_fd < 0, ESP_ERR_INVALID_STATE, TAG, "encoder already open");

	esp_err_t ret = ESP_OK;

	s_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDWR);
	ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG,
	                    "failed to open %s", ESP_VIDEO_H264_DEVICE_NAME);

	/* Encoder parameters must be set before the format, because setting the
	 * format is what creates the underlying encoder instance. */
	ESP_GOTO_ON_ERROR(set_codec_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, i_period),
	                  fail, TAG, "setting the I-frame period failed");
	ESP_GOTO_ON_ERROR(set_codec_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate),
	                  fail, TAG, "setting the bitrate failed");
	ESP_GOTO_ON_ERROR(set_codec_ctrl(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, APP_H264_MIN_QP),
	                  fail, TAG, "setting the minimum QP failed");
	ESP_GOTO_ON_ERROR(set_codec_ctrl(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, APP_H264_MAX_QP),
	                  fail, TAG, "setting the maximum QP failed");

	ESP_GOTO_ON_ERROR(set_queue_format(V4L2_BUF_TYPE_VIDEO_OUTPUT, width, height,
	                                   V4L2_PIX_FMT_YUV420),
	                  fail, TAG, "setting the encoder input format failed");
	ESP_GOTO_ON_ERROR(set_queue_format(V4L2_BUF_TYPE_VIDEO_CAPTURE, width, height,
	                                   V4L2_PIX_FMT_H264),
	                  fail, TAG, "setting the encoder output format failed");

	/* Input: USERPTR, so the camera buffer is passed by reference. */
	struct v4l2_requestbuffers out_req = {
		.count  = 1,
		.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_USERPTR,
	};
	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &out_req) == 0, ESP_FAIL,
	                  fail, TAG, "REQBUFS on the input queue failed");

	/* Output: MMAP, so the driver sizes the compressed buffer. */
	struct v4l2_requestbuffers cap_req = {
		.count  = 1,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &cap_req) == 0, ESP_FAIL,
	                  fail, TAG, "REQBUFS on the output queue failed");

	struct v4l2_buffer cap_buf = {
		.index  = 0,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &cap_buf) == 0, ESP_FAIL,
	                  fail, TAG, "QUERYBUF failed");

	s_cap_len = cap_buf.length;
	s_cap_buf = mmap(NULL, cap_buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
	                 s_fd, cap_buf.m.offset);
	ESP_GOTO_ON_FALSE(s_cap_buf != MAP_FAILED, ESP_ERR_NO_MEM, fail, TAG, "mmap failed");

	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &cap_buf) == 0, ESP_FAIL,
	                  fail, TAG, "queueing the output buffer failed");

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL,
	                  fail, TAG, "STREAMON on the output queue failed");
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ESP_GOTO_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL,
	                  fail, TAG, "STREAMON on the input queue failed");

	s_streaming = true;
	s_sps_len   = 0;
	s_pps_len   = 0;

	ESP_LOGI(TAG, "H.264 encoder ready: %" PRIu32 "x%" PRIu32 ", %" PRIu32
	              " bit/s, QP %d-%d, I-frame every %" PRIu32 " frames",
	         width, height, bitrate, APP_H264_MIN_QP, APP_H264_MAX_QP, i_period);
	return ESP_OK;

fail:
	app_h264_close();
	return ret;
}

void app_h264_close(void)
{
	if (s_fd < 0) {
		return;
	}

	if (s_streaming) {
		int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(s_fd, VIDIOC_STREAMOFF, &type);
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(s_fd, VIDIOC_STREAMOFF, &type);
		s_streaming = false;
	}

	if (s_cap_buf && s_cap_buf != MAP_FAILED) {
		munmap(s_cap_buf, s_cap_len);
	}
	s_cap_buf = NULL;
	s_cap_len = 0;

	close(s_fd);
	s_fd = -1;

	s_sps_len = 0;
	s_pps_len = 0;
}

bool app_h264_is_open(void)
{
	return s_fd >= 0 && s_streaming;
}

esp_err_t app_h264_encode(const uint8_t *yuv, size_t yuv_len, app_h264_frame_t *out)
{
	ESP_RETURN_ON_FALSE(app_h264_is_open(), ESP_ERR_INVALID_STATE, TAG, "encoder not open");
	ESP_RETURN_ON_FALSE(yuv && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

	struct v4l2_buffer in_buf = {
		.index     = 0,
		.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory    = V4L2_MEMORY_USERPTR,
		.m.userptr = (unsigned long)yuv,
		.length    = yuv_len,
		.bytesused = yuv_len,
	};
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &in_buf) == 0, ESP_FAIL, TAG,
	                    "queueing the raw frame failed");

	/* The M2M device runs the encode synchronously, so the compressed frame is
	 * ready as soon as the capture buffer comes back. */
	struct v4l2_buffer cap_buf = {
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_DQBUF, &cap_buf) == 0, ESP_FAIL, TAG,
	                    "dequeueing the encoded frame failed");

	struct v4l2_buffer done_in = {
		.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_USERPTR,
	};
	if (ioctl(s_fd, VIDIOC_DQBUF, &done_in) != 0) {
		ESP_LOGW(TAG, "dequeueing the raw frame failed");
	}

	esp_err_t err = ESP_OK;
	if (cap_buf.bytesused == 0) {
		ESP_LOGW(TAG, "encoder produced an empty frame");
		err = ESP_ERR_INVALID_STATE;
	} else {
		capture_parameter_sets(s_cap_buf, cap_buf.bytesused);
		out->data        = s_cap_buf;
		out->len         = cap_buf.bytesused;
		out->is_keyframe = contains_keyframe(s_cap_buf, cap_buf.bytesused);
	}

	/* Hand the buffer straight back; the caller is expected to have finished
	 * with the data (it is packetised and sent before the next encode). */
	struct v4l2_buffer requeue = {
		.index  = cap_buf.index,
		.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP,
	};
	if (ioctl(s_fd, VIDIOC_QBUF, &requeue) != 0) {
		ESP_LOGW(TAG, "requeueing the output buffer failed");
	}

	return err;
}

esp_err_t app_h264_get_parameter_sets(const uint8_t **sps, size_t *sps_len,
                                      const uint8_t **pps, size_t *pps_len)
{
	if (!s_sps_len || !s_pps_len) {
		return ESP_ERR_INVALID_STATE;
	}
	if (sps)     { *sps = s_sps; }
	if (sps_len) { *sps_len = s_sps_len; }
	if (pps)     { *pps = s_pps; }
	if (pps_len) { *pps_len = s_pps_len; }
	return ESP_OK;
}
