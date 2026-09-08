/*
 * PPA-based scaling and colour conversion for the epdInky camera example.
 *
 * The MJPEG preview needs RGB565, because this board's ESP32-P4 is silicon
 * revision v1.0 and its JPEG encoder only gained YUV420 input on v3.0. The PPA
 * does that colour conversion, and can downscale in the same pass if the
 * preview is ever served smaller than the sensor mode.
 *
 * IMPORTANT: the PPA and the hardware H.264 encoder cannot be used at the same
 * time. Once the encoder is open the PPA's completion interrupt never arrives
 * and ppa_do_scale_rotate_mirror() blocks forever. This was verified in
 * isolation - the PPA alone works, the encoder alone works, together they
 * deadlock on the first transaction. That is why the RTSP server only offers
 * the sensor's native resolution, and why nothing here may be called while a
 * stream is being encoded. The arbitration in app_camera.c enforces it.
 *
 * The output buffer is reallocated only when the requested size changes.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_scaler.h"

static const char *TAG = "app_scaler";

/* PPA output buffers must be cache-line aligned; MALLOC_CAP_CACHE_ALIGNED
 * makes the allocator handle both the address and the padded size. */
#define SCALER_MEM_CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED)

typedef struct {
	uint8_t *buf;
	size_t   size;      /* allocated bytes  */
	size_t   len;       /* bytes in use     */
	uint32_t width;
	uint32_t height;
} scaler_target_t;

static ppa_client_handle_t s_ppa;
static scaler_target_t     s_rgb;

/* ===========================================================================
 * Helpers
 * ========================================================================= */

static esp_err_t ensure_buffer(scaler_target_t *t, uint32_t width, uint32_t height,
                               size_t bytes_needed)
{
	if (t->buf && t->width == width && t->height == height) {
		return ESP_OK;
	}

	if (t->buf) {
		heap_caps_free(t->buf);
		t->buf  = NULL;
		t->size = 0;
	}

	t->buf = heap_caps_aligned_calloc(128, 1, bytes_needed, SCALER_MEM_CAPS);
	ESP_RETURN_ON_FALSE(t->buf, ESP_ERR_NO_MEM, TAG,
	                    "no memory for a %" PRIu32 "x%" PRIu32 " scaler buffer",
	                    width, height);

	t->size   = bytes_needed;
	t->width  = width;
	t->height = height;
	ESP_LOGI(TAG, "allocated a %" PRIu32 "x%" PRIu32 " buffer (%u bytes)",
	         width, height, (unsigned)bytes_needed);
	return ESP_OK;
}

static esp_err_t run_srm(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                         ppa_srm_color_mode_t src_cm,
                         scaler_target_t *dst, uint32_t dst_w, uint32_t dst_h,
                         ppa_srm_color_mode_t dst_cm)
{
	ESP_RETURN_ON_FALSE(s_ppa, ESP_ERR_INVALID_STATE, TAG, "scaler not initialised");

	/* The PPA rejects odd dimensions for YUV420 on either side. */
	ESP_RETURN_ON_FALSE((src_w % 2 == 0) && (src_h % 2 == 0) &&
	                    (dst_w % 2 == 0) && (dst_h % 2 == 0),
	                    ESP_ERR_INVALID_ARG, TAG, "dimensions must be even");

	ppa_srm_oper_config_t op = {
		.in = {
			.buffer         = src,
			.pic_w          = src_w,
			.pic_h          = src_h,
			.block_w        = src_w,
			.block_h        = src_h,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm         = src_cm,
			.yuv_range      = PPA_COLOR_RANGE_LIMIT,
			.yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
		},
		.out = {
			.buffer         = dst->buf,
			.buffer_size    = dst->size,
			.pic_w          = dst_w,
			.pic_h          = dst_h,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm         = dst_cm,
			.yuv_range      = PPA_COLOR_RANGE_LIMIT,
			.yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
		},
		.rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
		.scale_x        = (float)dst_w / (float)src_w,
		.scale_y        = (float)dst_h / (float)src_h,
		.mode           = PPA_TRANS_MODE_BLOCKING,
	};

	return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t app_scaler_init(void)
{
	if (s_ppa) {
		return ESP_OK;
	}

	ppa_client_config_t cfg = {
		.oper_type             = PPA_OPERATION_SRM,
		.max_pending_trans_num = 1,
	};
	ESP_RETURN_ON_ERROR(ppa_register_client(&cfg, &s_ppa), TAG,
	                    "failed to register the PPA client");

	ESP_LOGI(TAG, "PPA scaler ready");
	return ESP_OK;
}

void app_scaler_deinit(void)
{
	if (s_ppa) {
		ppa_unregister_client(s_ppa);
		s_ppa = NULL;
	}
	if (s_rgb.buf) {
		heap_caps_free(s_rgb.buf);
		memset(&s_rgb, 0, sizeof(s_rgb));
	}
}

esp_err_t app_scaler_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                            uint32_t dst_w, uint32_t dst_h,
                            const uint8_t **dst, size_t *dst_len)
{
	ESP_RETURN_ON_FALSE(src && dst && dst_len, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

	size_t needed = (size_t)dst_w * dst_h * 2;
	ESP_RETURN_ON_ERROR(ensure_buffer(&s_rgb, dst_w, dst_h, needed), TAG,
	                    "scaler buffer allocation failed");

	ESP_RETURN_ON_ERROR(run_srm(src, src_w, src_h, PPA_SRM_COLOR_MODE_YUV420,
	                            &s_rgb, dst_w, dst_h, PPA_SRM_COLOR_MODE_RGB565),
	                    TAG, "PPA convert failed");

	s_rgb.len = needed;
	*dst      = s_rgb.buf;
	*dst_len  = s_rgb.len;
	return ESP_OK;
}

esp_err_t app_scaler_yuv420_to_rgb888_rect(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                           void *dst, size_t dst_size,
                                           uint32_t dst_w, uint32_t dst_h,
                                           uint32_t x, uint32_t y,
                                           uint32_t w, uint32_t h)
{
	ESP_RETURN_ON_FALSE(s_ppa, ESP_ERR_INVALID_STATE, TAG, "scaler not initialised");
	ESP_RETURN_ON_FALSE(src && dst, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

	/*
	 * YUV420 input must be even in both axes, including its block offsets.
	 * The destination has no such constraint here: the PPA only enforces
	 * evenness on the output side when the output colour mode is itself YUV,
	 * and this one is RGB888. Requiring it anyway would reject legitimate
	 * letterbox heights such as 405.
	 */
	ESP_RETURN_ON_FALSE((src_w % 2 == 0) && (src_h % 2 == 0),
	                    ESP_ERR_INVALID_ARG, TAG, "YUV420 source must be even");
	ESP_RETURN_ON_FALSE((x + w <= dst_w) && (y + h <= dst_h),
	                    ESP_ERR_INVALID_ARG, TAG, "target rectangle is outside the image");

	ppa_srm_oper_config_t op = {
		.in = {
			.buffer         = src,
			.pic_w          = src_w,
			.pic_h          = src_h,
			.block_w        = src_w,
			.block_h        = src_h,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm         = PPA_SRM_COLOR_MODE_YUV420,
			.yuv_range      = PPA_COLOR_RANGE_LIMIT,
			.yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
		},
		.out = {
			.buffer         = dst,
			.buffer_size    = dst_size,
			.pic_w          = dst_w,
			.pic_h          = dst_h,
			.block_offset_x = x,
			.block_offset_y = y,
			.srm_cm         = PPA_SRM_COLOR_MODE_RGB888,
			.yuv_range      = PPA_COLOR_RANGE_LIMIT,
			.yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
		},
		.rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
		.scale_x        = (float)w / (float)src_w,
		.scale_y        = (float)h / (float)src_h,
		.mode           = PPA_TRANS_MODE_BLOCKING,
	};

	return ppa_do_scale_rotate_mirror(s_ppa, &op);
}
