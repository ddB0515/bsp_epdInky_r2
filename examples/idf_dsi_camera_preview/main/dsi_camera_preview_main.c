/*
 * epdInky ESP32-P4 - MIPI-CSI camera preview on the MIPI-DSI panel.
 *
 * The camera runs at 1280x720 YUV420 and the PPA scales and converts each
 * frame straight into the display's frame buffer as RGB565. Nothing is copied
 * in between: one hardware pass reads the camera buffer and writes the buffer
 * the panel is scanning out of.
 *
 * The panel is 1024x768 (4:3) and the camera is 16:9, so a full-width preview
 * is 1024x576 with a 96 pixel bar above and below. Those bars are deliberate -
 * they are where the UI lives, which keeps LVGL's redraws out of the rectangle
 * the PPA owns.
 *
 * CSI and DSI are separate peripherals, and the H.264 encoder is not used
 * here, so the PPA is free (it cannot run while that encoder is open).
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/epdinky_p4_board.h"

#include "app_camera.h"
#include "app_display.h"
#include "app_scaler.h"
#include "app_snapshot.h"
#include "app_stats.h"
#include "app_ui.h"

static const char *TAG = "dsi_preview";

/*
 * Sensor mode used for the preview.
 *
 * This is the one real trade-off in this example. The PPA converts YUV420 to
 * RGB888 and rescales in a single pass, so the cost is set by the SOURCE size
 * rather than the destination. Measured on this board, scaling into the
 * 800x450 preview window:
 *
 *   1920x1080 source -> 126.9 ms/frame ->  7.5 fps  (letterboxed 16:9)
 *   1280x720  source ->  48.9 ms/frame -> 15.0 fps  (letterboxed 16:9)
 *    640x480  source ->  13.4 ms/frame -> 37.0 fps  (pillarboxed 4:3)
 *
 * 720p is the default because it matches the window's 16:9 shape and gives a
 * good picture at a usable rate. Drop to 640x480 if a smoother viewfinder
 * matters more than detail, or pick 1080p for the most detail at 7.5 fps.
 */
#define PREVIEW_SENSOR_W 1280
#define PREVIEW_SENSOR_H  720

/* Show the DSI colour bars briefly at startup. They prove the link, the panel
 * and the backlight independently of the camera, which makes a bring-up
 * problem obvious at a glance. */
#define SHOW_TEST_PATTERN_MS 1500

/* Corner marker size for the orientation check. */
#define MARK 120

/*
 * Bring-up switches.
 *
 * The display is brought up on its own first: with the camera off, a blank
 * panel can only mean a display problem, which removes the ambiguity that
 * makes this hard to debug. Set ENABLE_CAMERA to 1 once the UI is visible.
 */
#define ENABLE_CAMERA          1
#define RUN_DISPLAY_DIAGNOSTIC 0

#if ENABLE_CAMERA
static void preview_task(void *arg)
{
	(void)arg;

	uint32_t src_w, src_h;
	app_camera_get_size(&src_w, &src_h);

	void  *fb      = app_display_frame_buffer();
	size_t fb_size = app_display_frame_buffer_size();


	uint32_t token = app_camera_stream_acquire(APP_STREAM_MJPEG,
	                                           PREVIEW_SENSOR_W, PREVIEW_SENSOR_H);
	app_camera_get_size(&src_w, &src_h);
	if (token == 0) {
		ESP_LOGE(TAG, "could not take the camera");
		vTaskDelete(NULL);
		return;
	}

	uint32_t frames = 0;
	uint32_t report = 0;
	int64_t  window = esp_timer_get_time();

	while (true) {
		/* Apply any resolution change between frames, never during one. */
		uint32_t want_w, want_h;
		if (app_ui_take_resolution_request(&want_w, &want_h)) {
			app_camera_stream_release(token);
			token = app_camera_stream_acquire(APP_STREAM_MJPEG, want_w, want_h);
			if (token == 0) {
				ESP_LOGE(TAG, "could not switch to %" PRIu32 "x%" PRIu32, want_w, want_h);
				vTaskDelay(pdMS_TO_TICKS(200));
				token = app_camera_stream_acquire(APP_STREAM_MJPEG, 0, 0);
				continue;
			}
			app_camera_get_size(&src_w, &src_h);
			ESP_LOGI(TAG, "Preview source now %" PRIu32 "x%" PRIu32, src_w, src_h);

			/* Blank the window before the new size arrives, so no part of the
			 * old image can survive underneath the new one. */
			app_display_fill_rect(APP_DISPLAY_VIDEO_X, APP_DISPLAY_VIDEO_Y,
			                      APP_DISPLAY_VIDEO_W, APP_DISPLAY_VIDEO_H, 0, 0, 0);
		}

		app_camera_frame_t frame;
		int64_t t_wait0 = esp_timer_get_time();
		if (app_camera_capture_yuv(&frame) != ESP_OK) {
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}
		int64_t t_wait = esp_timer_get_time() - t_wait0;

		/*
		 * Save a still, if one was asked for.
		 *
		 * Done here because this task already holds the camera: encoding
		 * from the UI task would contend for the same capture queue. The
		 * YUV frame in hand cannot be reused, as the SD card needs JPEG, so
		 * one encoded frame is captured for the file.
		 */
		if (app_ui_take_save_request()) {
			app_camera_release();

			app_camera_frame_t shot;
			esp_err_t serr = app_camera_capture_jpeg(src_w, src_h, &shot);
			if (serr == ESP_OK) {
				char path[64];
				serr = app_snapshot_write_sd(shot.data, shot.len, path, sizeof(path));
				if (serr == ESP_OK) {
					const char *name = strrchr(path, '/');
					ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)shot.len);
					app_ui_set_status(name ? name + 1 : path, true);
				} else {
					ESP_LOGE(TAG, "SD write failed: %s", esp_err_to_name(serr));
					app_ui_set_status("Write failed", false);
				}
				/* The encoded frame is checked out of the same queue the
				 * preview uses, so it has to go back before the next
				 * capture or every following frame fails. */
				app_camera_release();
			} else {
				ESP_LOGE(TAG, "JPEG capture failed: %s", esp_err_to_name(serr));
				app_ui_set_status("Capture failed", false);
			}
			continue;
		}

		/*
		 * Letterbox onto the PPA's own scaling grid.
		 *
		 * The PPA quantises the scale factor to 1/16 steps, and it derives
		 * the output size from that quantised value - not from the rectangle
		 * asked for. Requesting 800x450 from a 1920x1080 frame therefore
		 * writes only 720x405 and leaves the previous image showing along the
		 * right and bottom edges.
		 *
		 * Picking the scale in sixteenths first, then deriving the
		 * destination from it, means the rectangle written is exactly the
		 * rectangle intended. Sizes stay proportional, so 4:3 modes are
		 * pillarboxed rather than stretched.
		 */
		const uint32_t STEPS = 16;
		uint32_t sx = (uint32_t)((uint64_t)APP_DISPLAY_VIDEO_W * STEPS / frame.width);
		uint32_t sy = (uint32_t)((uint64_t)APP_DISPLAY_VIDEO_H * STEPS / frame.height);
		uint32_t s  = (sx < sy) ? sx : sy;
		if (s < 1) {
			s = 1;
		}

		int dst_w = (int)((uint64_t)frame.width * s / STEPS);
		int dst_h = (int)((uint64_t)frame.height * s / STEPS);
		int dst_x = APP_DISPLAY_VIDEO_X + (APP_DISPLAY_VIDEO_W - dst_w) / 2;
		int dst_y = APP_DISPLAY_VIDEO_Y + (APP_DISPLAY_VIDEO_H - dst_h) / 2;

		/* Clear the surrounding bars once when the shape changes, not every
		 * frame: the PPA only ever writes the video rectangle itself. */
		static int last_w, last_h;
		if (dst_w != last_w || dst_h != last_h) {
			app_display_fill_rect(APP_DISPLAY_VIDEO_X, APP_DISPLAY_VIDEO_Y,
			                      APP_DISPLAY_VIDEO_W, APP_DISPLAY_VIDEO_H, 0, 0, 0);
			last_w = dst_w;
			last_h = dst_h;
		}

		int64_t t0 = esp_timer_get_time();
		esp_err_t err = app_scaler_yuv420_to_rgb888_rect(
			frame.data, frame.width, frame.height,
			fb, fb_size,
			BSP_DSI_LCD_H_RES, BSP_DSI_LCD_V_RES,
			dst_x, dst_y,
			dst_w, dst_h);

		int64_t t_scale = esp_timer_get_time() - t0;
		app_camera_release();

		static int64_t scale_total; static uint32_t scale_n;
		static int64_t wait_total;
		scale_total += t_scale; scale_n++;
		wait_total += t_wait;

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "scale failed: %s", esp_err_to_name(err));
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		/* Refresh the on-screen rate about once a second; log less often. */
		frames++;
		int64_t now = esp_timer_get_time();
		if (now - window >= 1000000) {
			float fps = frames / ((now - window) / 1e6);
			app_ui_set_fps(fps);
			app_ui_set_luma(app_camera_get_measured_luma());

			if (++report >= 10) {
				double ppa_ms  = scale_n ? (scale_total / (double)scale_n) / 1000.0 : 0.0;
				double wait_ms = scale_n ? (wait_total  / (double)scale_n) / 1000.0 : 0.0;
				ESP_LOGI(TAG, "Preview %.1f fps | PPA %.2f ms | camera wait %.2f ms "
				              "| loop %.2f ms | src %" PRIu32 "x%" PRIu32,
				         fps, ppa_ms, wait_ms, ppa_ms + wait_ms, src_w, src_h);
				report = 0;
			}
			scale_total = 0; scale_n = 0; wait_total = 0;
			frames = 0;
			window = now;
		}
	}
}
#endif /* ENABLE_CAMERA */

void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4 DSI camera preview");

	/* The camera needs I2C for SCCB and the backlight controller is on the
	 * same bus, so the expander is the only other thing worth having. */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tps65185 = false;
	cfg.enable.use_kxtj3    = false;
	cfg.enable.use_rv3028   = false;
	cfg.enable.use_stc3115  = false;
	cfg.enable.use_epd_gpio = false;
	cfg.enable.use_sdcard   = false;   /* mounted by hand below, so a missing
	                                    * card is a warning rather than fatal */

	bsp_epdinky_handles_t board;
	ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));

	/* Show what is actually on the bus before touching the display, so a
	 * missing panel is obvious rather than looking like a driver fault. */
	bsp_i2c_scan();

	/*
	 * Mount the SD card for stills.
	 *
	 * A missing or unreadable card is not fatal: the save button is simply
	 * disabled. Retried once because the card is power-cycled during board
	 * init and does not always answer the first attempt.
	 */
	esp_err_t sd_err = bsp_sdcard_mount(&cfg, board.tca6408);
	if (sd_err != ESP_OK) {
		vTaskDelay(pdMS_TO_TICKS(250));
		sd_err = bsp_sdcard_mount(&cfg, board.tca6408);
	}
	if (sd_err != ESP_OK) {
		ESP_LOGW(TAG, "No SD card (%s); saving stills will be unavailable",
		         esp_err_to_name(sd_err));
	}
	ESP_ERROR_CHECK(app_snapshot_init());
	ESP_ERROR_CHECK(app_stats_init());

	esp_err_t err = app_display_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
		ESP_LOGE(TAG, "Check the D320C2403V1 panel is seated in FPC1.");
		return;
	}

	/* Matches the slider's starting position, so the two agree at boot. */
	ESP_ERROR_CHECK(app_display_set_backlight(APP_UI_BRIGHTNESS_DEFAULT));

#if RUN_DISPLAY_DIAGNOSTIC
	/*
	 * Staged bring-up check, kept for when the panel misbehaves. Whichever
	 * step first fails to appear is where the fault is.
	 */
	ESP_LOGI(TAG, "DIAG: DSI colour bars (proves link + panel + backlight)");
	ESP_ERROR_CHECK(app_display_test_pattern(true));
	vTaskDelay(pdMS_TO_TICKS(2000));
	ESP_ERROR_CHECK(app_display_test_pattern(false));

	ESP_LOGI(TAG, "DIAG: solid RED, GREEN, BLUE from the frame buffer");
	app_display_fill(255, 0, 0);
	vTaskDelay(pdMS_TO_TICKS(1500));
	app_display_fill(0, 255, 0);
	vTaskDelay(pdMS_TO_TICKS(1500));
	app_display_fill(0, 0, 255);
	vTaskDelay(pdMS_TO_TICKS(1500));

	/* Asymmetric in both axes, so orientation and mirroring are unambiguous.
	 * Also exercises rectangle writes, the path LVGL and the PPA use. */
	ESP_LOGI(TAG, "DIAG: red=top-left green=top-right blue=bottom-left "
	              "white stripe=top edge, bottom-right black");
	app_display_fill(0, 0, 0);
	app_display_fill_rect(0, 0, MARK, MARK, 255, 0, 0);
	app_display_fill_rect(BSP_DSI_LCD_H_RES - MARK, 0, MARK, MARK, 0, 255, 0);
	app_display_fill_rect(0, BSP_DSI_LCD_V_RES - MARK, MARK, MARK, 0, 0, 255);
	app_display_fill_rect(0, 0, BSP_DSI_LCD_H_RES, 16, 255, 255, 255);
	vTaskDelay(pdMS_TO_TICKS(4000));
#endif

	app_display_fill(0, 0, 0);

	/*
	 * Confirms the panel is actually being refreshed from the frame buffer.
	 *
	 * Cheap and worth keeping: the DSI colour bars are generated in the host
	 * downstream of the bridge, so they render even when frame buffer
	 * scan-out is dead. VSYNC is the only signal that tells them apart, and
	 * it is what caught the wrong lane rate during bring-up.
	 */
	app_display_selftest();

	/*
	 * LVGL first, camera second.
	 *
	 * The UI must be visible on its own before video is layered on top,
	 * otherwise a blank panel is ambiguous between a display fault and a
	 * camera fault.
	 */
	err = app_ui_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
		return;
	}
	ESP_LOGI(TAG, "LVGL is up - the interface should now be on the panel");

#if ENABLE_CAMERA
	err = app_camera_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
		ESP_LOGE(TAG, "Check that the SC2336 module is seated in the CSI connector.");
		return;
	}

	uint32_t w = 0, h = 0;
	app_camera_get_size(&w, &h);

	ESP_LOGI(TAG, "=====================================================");
	ESP_LOGI(TAG, " Camera  %" PRIu32 "x%" PRIu32 " YUV420", w, h);
	ESP_LOGI(TAG, " Panel   %dx%d RGB888", BSP_DSI_LCD_H_RES, BSP_DSI_LCD_V_RES);
	ESP_LOGI(TAG, " Preview %dx%d at (%d,%d), scaled by the PPA",
	         APP_DISPLAY_VIDEO_W, APP_DISPLAY_VIDEO_H,
	         APP_DISPLAY_VIDEO_X, APP_DISPLAY_VIDEO_Y);
	ESP_LOGI(TAG, "=====================================================");

	xTaskCreate(preview_task, "preview", 4096, NULL, 5, NULL);
#else
	ESP_LOGW(TAG, "Camera disabled (ENABLE_CAMERA 0) - LVGL bring-up only");
#endif

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}
