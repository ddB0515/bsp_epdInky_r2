/*
 * Still capture for the epdInky camera example.
 *
 * One captured image can go to either destination - written to the SD card, or
 * sent to the browser as a download - so capture is separated from what
 * happens to the bytes afterwards.
 *
 * There are two ways a still is produced, and the difference matters:
 *
 *   - While the MJPEG stream is running, the streaming loop already holds a
 *     freshly encoded JPEG, so the request asks it to hand the next frame
 *     over. Nothing competes for the camera, and the image is exactly what the
 *     viewer just saw.
 *
 *   - When nothing is streaming, this takes the camera itself for a single
 *     frame and gives it straight back. The same path covers a stream that
 *     does not answer in time, which happens for a moment after a viewer
 *     disconnects: the caller's "streaming" flag stays set until the handler
 *     unwinds, so a still asked for in that window falls back here instead of
 *     failing.
 *
 * The first case is why this is not simply "capture and save": the camera has
 * one capture queue and one JPEG output buffer, so a second consumer trying to
 * dequeue in parallel would fail, or race over the same memory.
 *
 * The streaming loop only ever copies the frame here; the slow work (an SD
 * write, or a socket send) happens on the requesting task, so a still never
 * stalls the stream for longer than a memcpy.
 *
 * Files are written as /sdcard/snapNNNN.jpg. That name is 8.3-safe on purpose,
 * so it works whether or not FATFS long filename support is enabled.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "bsp/config.h"

#include "app_camera.h"
#include "app_snapshot.h"

static const char *TAG = "app_snap";

/* A streamed frame arrives every ~33 ms, so this is already generous. If it
 * expires the still is captured directly instead of failing, so there is no
 * reason to wait longer. */
#define SNAPSHOT_WAIT_MS 1000

#define SNAPSHOT_PATH_MAX 64

/* Frames discarded before an idle capture, to let auto-exposure settle. At
 * ~33 ms each this costs about a fifth of a second, well worth it: the first
 * frame after an idle period is often mis-exposed and noisy, which produces a
 * file several times larger because noise does not compress. */
#define SNAPSHOT_WARMUP_FRAMES 6

/* Held from app_snapshot_begin() to app_snapshot_end(), so the image buffer
 * cannot be overwritten while a caller is still using it. */
static SemaphoreHandle_t s_lock;
/* Signalled by the streaming loop once it has copied the pending frame. */
static SemaphoreHandle_t s_done;

static volatile bool s_pending;
static bool          s_sd_available;
static uint32_t      s_next_index = 1;

/* The captured image, copied out of the encoder's buffer. */
static uint8_t *s_buf;
static size_t   s_buf_size;
static size_t   s_len;

/* ===========================================================================
 * Image buffer
 * ========================================================================= */

static esp_err_t store_image(const uint8_t *jpeg, size_t len)
{
	if (len > s_buf_size) {
		uint8_t *grown = heap_caps_realloc(s_buf, len,
		                                   MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
		if (!grown) {
			ESP_LOGE(TAG, "no memory for a %u byte still", (unsigned)len);
			return ESP_ERR_NO_MEM;
		}
		s_buf      = grown;
		s_buf_size = len;
	}

	memcpy(s_buf, jpeg, len);
	s_len = len;
	return ESP_OK;
}

/* ===========================================================================
 * Files
 * ========================================================================= */

/*
 * Pick the next unused filename.
 *
 * The index persists across calls so a long session does not rescan from zero
 * every time, but each candidate is still checked, so restarting the board
 * will not overwrite existing shots.
 */
static esp_err_t next_free_path(char *path, size_t path_size)
{
	for (uint32_t i = s_next_index; i < 10000; i++) {
		snprintf(path, path_size, BSP_SD_MOUNT_POINT "/snap%04" PRIu32 ".jpg", i);

		struct stat st;
		if (stat(path, &st) != 0) {
			s_next_index = i + 1;
			return ESP_OK;
		}
	}
	ESP_LOGE(TAG, "no free filenames left on the card");
	return ESP_ERR_NO_MEM;
}

/*
 * Write the JPEG, then read it back to confirm what landed on the card.
 *
 * The read-back is cheap - four bytes and a size check - and catches the
 * failures that matter for removable media: a short write, a card that
 * silently dropped the data, or a file left truncated. A JPEG always starts
 * with SOI (FF D8) and ends with EOI (FF D9), so those markers plus the length
 * are enough to say the file is complete rather than merely created.
 */
static esp_err_t write_and_verify(const char *path, const uint8_t *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "could not open %s for writing", path);

	size_t written = fwrite(data, 1, len, f);
	int    flushed = fflush(f);
	fclose(f);

	if (written != len || flushed != 0) {
		ESP_LOGE(TAG, "short write to %s (%u of %u bytes)",
		         path, (unsigned)written, (unsigned)len);
		remove(path);
		return ESP_FAIL;
	}

	f = fopen(path, "rb");
	ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "could not reopen %s to verify", path);

	uint8_t soi[2] = {0}, eoi[2] = {0};
	bool ok = (fread(soi, 1, 2, f) == 2) &&
	          (fseek(f, -2, SEEK_END) == 0) &&
	          (fread(eoi, 1, 2, f) == 2);
	long size = (ok && fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
	fclose(f);

	if (!ok || size != (long)len || soi[0] != 0xFF || soi[1] != 0xD8 ||
	    eoi[0] != 0xFF || eoi[1] != 0xD9) {
		ESP_LOGE(TAG, "%s did not verify (size %ld of %u, SOI %02X%02X, EOI %02X%02X)",
		         path, size, (unsigned)len, soi[0], soi[1], eoi[0], eoi[1]);
		remove(path);
		return ESP_FAIL;
	}
	return ESP_OK;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t app_snapshot_init(void)
{
	s_lock = xSemaphoreCreateMutex();
	s_done = xSemaphoreCreateBinary();
	ESP_RETURN_ON_FALSE(s_lock && s_done, ESP_ERR_NO_MEM, TAG,
	                    "could not create the snapshot primitives");

	struct stat st;
	if (stat(BSP_SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
		ESP_LOGW(TAG, "%s is not mounted; stills can still be downloaded",
		         BSP_SD_MOUNT_POINT);
		s_sd_available = false;
		return ESP_OK;
	}

	s_sd_available = true;
	ESP_LOGI(TAG, "Stills enabled, SD path %s/snapNNNN.jpg", BSP_SD_MOUNT_POINT);
	return ESP_OK;
}

bool app_snapshot_sd_available(void)
{
	return s_sd_available;
}

void app_snapshot_offer_frame(const uint8_t *jpeg, size_t len)
{
	if (!s_pending) {
		return;
	}

	/* Copy only - the requester does the slow work. */
	esp_err_t err = store_image(jpeg, len);
	if (err != ESP_OK) {
		s_len = 0;
	}

	s_pending = false;
	xSemaphoreGive(s_done);
}

esp_err_t app_snapshot_begin(bool stream_running, const uint8_t **data, size_t *len)
{
	ESP_RETURN_ON_FALSE(data && len, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
	ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");

	xSemaphoreTake(s_lock, portMAX_DELAY);
	esp_err_t err = ESP_OK;
	bool got_frame = false;
	s_len = 0;

	if (stream_running) {
		/* Ask the streaming loop for its next frame. Clear any stale
		 * completion first, so we cannot observe a previous one. */
		xSemaphoreTake(s_done, 0);
		s_pending = true;

		if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SNAPSHOT_WAIT_MS)) == pdTRUE && s_len) {
			got_frame = true;
		} else {
			/* The caller's view of "streaming" can be a moment out of date -
			 * a client that has just disconnected leaves the flag set until
			 * the handler unwinds - so fall back to taking the picture here
			 * rather than failing. */
			s_pending = false;
			ESP_LOGD(TAG, "no frame from the stream, capturing directly");
		}
	}

	if (!got_frame) {
		/* 0,0 keeps whatever mode is current - a still should not change it. */
		uint32_t token = app_camera_stream_acquire(APP_STREAM_MJPEG, 0, 0);
		if (token == 0) {
			xSemaphoreGive(s_lock);
			ESP_LOGW(TAG, "could not take the camera for a still");
			return ESP_ERR_INVALID_STATE;
		}

		app_camera_frame_t frame;
		for (int i = 0; i < SNAPSHOT_WARMUP_FRAMES && err == ESP_OK; i++) {
			err = app_camera_capture_jpeg(0, 0, &frame);
			if (err == ESP_OK) {
				app_camera_release();
			}
		}

		if (err == ESP_OK) {
			err = app_camera_capture_jpeg(0, 0, &frame);
			if (err == ESP_OK) {
				err = store_image(frame.data, frame.len);
				app_camera_release();
			}
		}
		app_camera_stream_release(token);

		if (err != ESP_OK) {
			xSemaphoreGive(s_lock);
			return err;
		}
	}

	*data = s_buf;
	*len  = s_len;
	return ESP_OK;   /* lock stays held until app_snapshot_end() */
}

void app_snapshot_end(void)
{
	if (s_lock) {
		xSemaphoreGive(s_lock);
	}
}

esp_err_t app_snapshot_write_sd(const uint8_t *data, size_t len,
                                char *path, size_t path_size)
{
	ESP_RETURN_ON_FALSE(data && len && path, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
	ESP_RETURN_ON_FALSE(s_sd_available, ESP_ERR_NOT_FOUND, TAG, "no SD card mounted");

	ESP_RETURN_ON_ERROR(next_free_path(path, path_size), TAG, "naming failed");
	ESP_RETURN_ON_ERROR(write_and_verify(path, data, len), TAG, "write failed");

	ESP_LOGI(TAG, "saved %s (%u bytes)", path, (unsigned)len);
	return ESP_OK;
}
