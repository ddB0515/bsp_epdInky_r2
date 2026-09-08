/*
 * LVGL interface for the camera preview.
 *
 * The layout is the important part. The camera occupies a 1024x576 window in
 * the middle of the 1024x768 panel, written straight into the frame buffer by
 * the PPA, and LVGL only ever owns the 96-pixel bars above and below it. Two
 * writers share one frame buffer, and that works only because they never
 * overlap: every widget lives in a bar, so an LVGL redraw can never land on a
 * pixel the PPA is writing, and neither has to wait for the other.
 *
 * LVGL is driven directly rather than through esp_lvgl_port. That component
 * does not currently compile against ESP-IDF 6.0 (it sets
 * cbs.on_frame_buf_complete, which 6.0 renamed back to on_refresh_done), and
 * doing it by hand is only a few dozen lines while giving exact control over
 * which pixels get touched.
 *
 * Touch is a GT967: a Goodix GT9xx part that speaks the same register protocol
 * as the GT911, so the stock esp_lcd_touch_gt911 driver drives it.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "linux/videodev2.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "app_camera.h"
#include "app_snapshot.h"
#include "app_stats.h"
#include "app_display.h"
#include "app_ui.h"

static const char *TAG = "app_ui";

/* LVGL only ever redraws the bars, so a buffer a few lines tall is plenty. */
#define LVGL_DRAW_LINES 64
#define LVGL_TICK_MS     5
#define LVGL_TASK_STACK  6144

static lv_display_t          *s_disp;
static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t      s_lock;

static lv_obj_t *s_fps_label;
static lv_obj_t *s_luma_label;

/* Sensor modes offered on the side panel. All are real SC2336 modes, so the
 * preview never has to be faked by scaling something the sensor cannot do.
 * Ordered smallest to largest, which is also fastest to slowest: the PPA cost
 * tracks the source pixel count, so 1920x1080 runs noticeably slower. */
static const app_ui_res_t s_res[] = {
	{  640,  480,  "640x480"  },
	{ 1280,  720, "1280x720" },
	{ 1920, 1080, "1920x1080" },
};
#define RES_COUNT (sizeof(s_res) / sizeof(s_res[0]))
static const size_t s_res_count  = RES_COUNT;
static size_t       s_res_active = 1;          /* 1280x720 by default */
static lv_obj_t    *s_res_btns[RES_COUNT];

/* Set by the UI, acted on by the preview task between frames. */
static volatile uint32_t s_req_w, s_req_h;
static volatile bool     s_req_save;
static lv_obj_t         *s_status_label;
static lv_obj_t         *s_save_btn;
static lv_obj_t         *s_stats_label;
static volatile uint32_t s_lvgl_renders;

/* ===========================================================================
 * LVGL plumbing
 * ========================================================================= */

bool app_ui_lock(uint32_t timeout_ms)
{
	if (!s_lock) {
		return false;
	}
	TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
	return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void app_ui_unlock(void)
{
	if (s_lock) {
		xSemaphoreGiveRecursive(s_lock);
	}
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

	/*
	 * Hand the rendered block to the DPI and return without calling
	 * lv_display_flush_ready(): the transfer is asynchronous, so LVGL must
	 * not reuse the buffer until on_trans_done() fires.
	 */
	esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
	                          area->x2 + 1, area->y2 + 1, px_map);
}

static bool on_trans_done(esp_lcd_panel_handle_t panel,
                          esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
	(void)panel;
	(void)edata;
	lv_display_flush_ready((lv_display_t *)user_ctx);
	return false;
}

static void tick_cb(void *arg)
{
	(void)arg;
	lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
	(void)arg;
	while (true) {
		uint32_t wait_ms = 10;
		if (app_ui_lock(0)) {
			wait_ms = lv_timer_handler();
			app_ui_unlock();
		}
		if (wait_ms > 100) {
			wait_ms = 100;
		}
		vTaskDelay(pdMS_TO_TICKS(wait_ms < 5 ? 5 : wait_ms));
	}
}

/* ===========================================================================
 * Touch
 * ========================================================================= */

static esp_err_t touch_init(void)
{
	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "I2C bus unavailable");

	esp_lcd_panel_io_handle_t io = NULL;
	esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
	io_cfg.dev_addr = BSP_I2C_ADDR_TOUCH;
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch IO failed");

	const esp_lcd_touch_config_t cfg = {
		.x_max        = BSP_DSI_LCD_H_RES,
		.y_max        = BSP_DSI_LCD_V_RES,
		.rst_gpio_num = BSP_DSI_PIN_TOUCH_RST,
		.int_gpio_num = BSP_DSI_PIN_TOUCH_INT,
		.levels = { .reset = 0, .interrupt = 0 },
		.flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
	};

	esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_touch);
	if (err != ESP_OK) {
		esp_lcd_panel_io_del(io);
	}
	return err;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	uint16_t x = 0, y = 0;
	uint8_t  count = 0;

	esp_lcd_touch_read_data(s_touch);
	bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &count, 1);

	if (pressed && count > 0) {
		data->point.x = x;
		data->point.y = y;
		data->state   = LV_INDEV_STATE_PRESSED;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

/* ===========================================================================
 * Widgets
 * ========================================================================= */

static void mirror_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	app_camera_set_control(V4L2_CID_HFLIP, on ? 1 : 0);
}

static void vflip_cb(lv_event_t *e)
{
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	app_camera_set_control(V4L2_CID_VFLIP, on ? 1 : 0);
}

static void brightness_cb(lv_event_t *e)
{
	app_display_set_backlight((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

/*
 * Resolution buttons.
 *
 * The switch itself is done by the preview task rather than here: changing the
 * sensor mode tears the capture down and rebuilds it, which must not happen
 * while a frame is being scaled. Setting a request and letting the task act on
 * it between frames keeps that safe.
 */
static void res_cb(lv_event_t *e)
{
	const app_ui_res_t *want = lv_event_get_user_data(e);
	app_ui_request_resolution(want->w, want->h);

	/* Show the selection immediately; the task confirms it a frame later. */
	for (size_t i = 0; i < s_res_count; i++) {
		if (s_res_btns[i]) {
			if (&s_res[i] == want) {
				lv_obj_add_state(s_res_btns[i], LV_STATE_CHECKED);
			} else {
				lv_obj_remove_state(s_res_btns[i], LV_STATE_CHECKED);
			}
		}
	}
}

/*
 * Bottom-right stats overlay.
 *
 * "LVGL fps" counts real render passes, not refresh ticks: LV_EVENT_REFR_READY
 * fires on every refresh period even when nothing was redrawn, so it would
 * report a steady 30 and mean nothing. A low number here is correct and
 * expected - the PPA writes the video straight into the frame buffer, so LVGL
 * only renders when a label or a button actually changes.
 */
static void render_ready_cb(lv_event_t *e)
{
	(void)e;
	s_lvgl_renders++;
}

static void stats_timer_cb(lv_timer_t *t)
{
	(void)t;
	if (!s_stats_label) {
		return;
	}

	uint32_t renders = s_lvgl_renders;
	s_lvgl_renders = 0;

	float busy[APP_STATS_CORES];
	if (app_stats_cpu(busy) != ESP_OK) {
		return;
	}

	/* Integer formatting: LVGL's label formatting uses picolibc's
	 * integer-only vsnprintf, so "%.1f" would render as nothing. */
	lv_label_set_text_fmt(s_stats_label, "CPU %d/%d%%  LVGL %" LV_PRIu32 " fps",
	                      (int)(busy[0] + 0.5f), (int)(busy[1] + 0.5f), renders);

	/* Mirror to the log every 10 s, so the same numbers are available over
	 * serial when nobody is looking at the panel. */
	static uint32_t tick;
	if (++tick >= 10) {
		tick = 0;
		ESP_LOGI(TAG, "CPU0 %.0f%%  CPU1 %.0f%%  LVGL %" PRIu32 " renders/s",
		         busy[0], busy[1], renders);
	}
}

static void save_cb(lv_event_t *e)
{
	(void)e;
	/* The preview task owns the camera, so it does the capture and the write;
	 * this only raises the request and reports that it was heard. */
	app_ui_request_save();
	app_ui_set_status("Saving...", true);
}

static lv_obj_t *make_res_button(lv_obj_t *parent, const app_ui_res_t *res, int y)
{
	lv_obj_t *btn = lv_button_create(parent);
	lv_obj_set_size(btn, APP_DISPLAY_SIDE_W - 32, 64);
	lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_add_event_cb(btn, res_cb, LV_EVENT_CLICKED, (void *)res);

	/* Grey when inactive, green when selected. The default theme only shifts
	 * the shade slightly between states, which is hard to read at a glance. */
	lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_GREY, 2), LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), LV_STATE_CHECKED);
	lv_obj_set_style_text_color(btn, lv_color_white(), LV_STATE_DEFAULT);
	lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_CHECKED);

	lv_obj_t *lab = lv_label_create(btn);
	lv_label_set_text(lab, res->label);
	lv_obj_center(lab);
	return btn;
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* --- title strip --- */
	lv_obj_t *title_bar = lv_obj_create(scr);
	lv_obj_set_size(title_bar, BSP_DSI_LCD_H_RES, APP_DISPLAY_TITLE_H);
	lv_obj_set_pos(title_bar, 0, 0);
	lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x1d2733), 0);
	lv_obj_set_style_border_width(title_bar, 0, 0);
	lv_obj_set_style_radius(title_bar, 0, 0);
	lv_obj_set_style_pad_all(title_bar, 12, 0);
	lv_obj_remove_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(title_bar);
	lv_label_set_text(title, LV_SYMBOL_IMAGE "  DSI + SC2336 Camera");
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

	s_fps_label = lv_label_create(title_bar);
	lv_label_set_text(s_fps_label, "-- fps");
	lv_obj_set_style_text_color(s_fps_label, lv_palette_main(LV_PALETTE_GREEN), 0);
	lv_obj_align(s_fps_label, LV_ALIGN_RIGHT_MID, 0, 0);

	s_luma_label = lv_label_create(title_bar);
	lv_label_set_text(s_luma_label, "luma --");
	lv_obj_set_style_text_color(s_luma_label, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_align(s_luma_label, LV_ALIGN_RIGHT_MID, -140, 0);

	/*
	 * Frame around the preview.
	 *
	 * Only the border is drawn - the inside is left transparent, because the
	 * PPA is writing there. Painting a background would fight the video and
	 * show as flicker.
	 */
	lv_obj_t *frame = lv_obj_create(scr);
	lv_obj_set_size(frame, APP_DISPLAY_VIDEO_W + 8, APP_DISPLAY_VIDEO_H + 8);
	lv_obj_set_pos(frame, APP_DISPLAY_VIDEO_X - 4, APP_DISPLAY_VIDEO_Y - 4);
	lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_color(frame, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
	lv_obj_set_style_border_width(frame, 2, 0);
	lv_obj_set_style_radius(frame, 4, 0);
	lv_obj_set_style_pad_all(frame, 0, 0);
	lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);

	/* --- side panel: resolution --- */
	lv_obj_t *side = lv_obj_create(scr);
	lv_obj_set_size(side, APP_DISPLAY_SIDE_W, BSP_DSI_LCD_V_RES - APP_DISPLAY_TITLE_H);
	lv_obj_set_pos(side, BSP_DSI_LCD_H_RES - APP_DISPLAY_SIDE_W, APP_DISPLAY_TITLE_H);
	lv_obj_set_style_bg_color(side, lv_color_hex(0x161c24), 0);
	lv_obj_set_style_border_width(side, 0, 0);
	lv_obj_set_style_radius(side, 0, 0);
	lv_obj_set_style_pad_all(side, 16, 0);
	lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *rlab = lv_label_create(side);
	lv_label_set_text(rlab, "Resolution");
	lv_obj_set_style_text_color(rlab, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_align(rlab, LV_ALIGN_TOP_MID, 0, 0);

	for (size_t i = 0; i < s_res_count; i++) {
		s_res_btns[i] = make_res_button(side, &s_res[i], 40 + (int)i * 76);
	}
	/* The default mode starts selected. */
	if (s_res_btns[s_res_active]) {
		lv_obj_add_state(s_res_btns[s_res_active], LV_STATE_CHECKED);
	}

	/* --- side panel: mirror and brightness --- */
	lv_obj_t *mirror = lv_switch_create(side);
	lv_obj_align(mirror, LV_ALIGN_TOP_LEFT, 0, 280);
	lv_obj_add_event_cb(mirror, mirror_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *mlab = lv_label_create(side);
	lv_label_set_text(mlab, "Mirror");
	lv_obj_set_style_text_color(mlab, lv_color_white(), 0);
	lv_obj_align_to(mlab, mirror, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

	lv_obj_t *vflip = lv_switch_create(side);
	lv_obj_align(vflip, LV_ALIGN_TOP_LEFT, 0, 330);
	lv_obj_add_event_cb(vflip, vflip_cb, LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *vlab = lv_label_create(side);
	lv_label_set_text(vlab, "VFlip");
	lv_obj_set_style_text_color(vlab, lv_color_white(), 0);
	lv_obj_align_to(vlab, vflip, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

	lv_obj_t *blab = lv_label_create(side);
	lv_label_set_text(blab, LV_SYMBOL_SETTINGS " LCD Brightness");
	lv_obj_set_style_text_color(blab, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_align(blab, LV_ALIGN_TOP_MID, 0, 385);

	lv_obj_t *slider = lv_slider_create(side);
	lv_obj_set_width(slider, APP_DISPLAY_SIDE_W - 48);
	lv_slider_set_range(slider, 5, 100);
	lv_slider_set_value(slider, APP_UI_BRIGHTNESS_DEFAULT, LV_ANIM_OFF);
	lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 420);
	lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

	/* --- save to SD --- */
	s_save_btn = lv_button_create(side);
	lv_obj_set_size(s_save_btn, APP_DISPLAY_SIDE_W - 32, 64);
	lv_obj_align(s_save_btn, LV_ALIGN_TOP_MID, 0, 470);
	lv_obj_add_event_cb(s_save_btn, save_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_bg_color(s_save_btn, lv_palette_darken(LV_PALETTE_BLUE, 1),
	                          LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(s_save_btn, lv_palette_darken(LV_PALETTE_GREY, 3),
	                          LV_STATE_DISABLED);

	lv_obj_t *slab = lv_label_create(s_save_btn);
	lv_label_set_text(slab, LV_SYMBOL_SD_CARD "  Save to SD");
	lv_obj_center(slab);

	s_status_label = lv_label_create(side);
	lv_label_set_text(s_status_label, "");
	lv_obj_set_style_text_color(s_status_label, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 542);

	/* Without a card the button would just fail on every press, so say so up
	 * front rather than letting it look broken. */
	if (!app_snapshot_sd_available()) {
		lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
		lv_label_set_text(s_status_label, "No SD card");
	}

	/* --- stats overlay, bottom right --- */
	s_stats_label = lv_label_create(scr);
	lv_label_set_text(s_stats_label, "CPU --/--%  LVGL -- fps");
	lv_obj_set_style_text_font(s_stats_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(s_stats_label, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_set_style_bg_color(s_stats_label, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s_stats_label, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s_stats_label, 4, 0);
	lv_obj_align(s_stats_label, LV_ALIGN_BOTTOM_RIGHT, -6, -6);

	lv_timer_create(stats_timer_cb, 1000, NULL);
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t app_ui_init(void)
{
	s_lock = xSemaphoreCreateRecursiveMutex();
	ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "could not create the LVGL mutex");

	lv_init();

	s_disp = lv_display_create(BSP_DSI_LCD_H_RES, BSP_DSI_LCD_V_RES);
	ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "could not create the LVGL display");

	lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB888);
	lv_display_set_user_data(s_disp, app_display_panel());
	lv_display_set_flush_cb(s_disp, flush_cb);
	lv_display_add_event_cb(s_disp, render_ready_cb, LV_EVENT_RENDER_READY, NULL);

	/*
	 * PARTIAL mode with two PSRAM draw buffers, matching the configuration
	 * this panel is known to work with. LVGL renders a block, the DPI copies
	 * it in asynchronously, and LVGL draws the next block into the other
	 * buffer meanwhile.
	 */
	size_t bpp = lv_color_format_get_size(lv_display_get_color_format(s_disp));
	size_t buf_bytes = (size_t)BSP_DSI_LCD_H_RES * LVGL_DRAW_LINES * bpp;

	void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
	void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
	ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG,
	                    "no memory for the LVGL draw buffers");
	lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

	esp_lcd_dpi_panel_event_callbacks_t cbs = {
		.on_color_trans_done = on_trans_done,
	};	ESP_RETURN_ON_ERROR(
	    esp_lcd_dpi_panel_register_event_callbacks(app_display_panel(), &cbs, s_disp),
	    TAG, "could not register the DPI flush-ready callback");

	/* Touch is optional - the preview is still useful without it. */
	if (touch_init() == ESP_OK) {
		lv_indev_t *indev = lv_indev_create();
		lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
		lv_indev_set_display(indev, s_disp);
		lv_indev_set_read_cb(indev, touch_read_cb);
		ESP_LOGI(TAG, "GT967 touch ready at 0x%02X", BSP_I2C_ADDR_TOUCH);
	} else {
		ESP_LOGW(TAG, "No touch at 0x%02X - the UI will be display-only",
		         BSP_I2C_ADDR_TOUCH);
	}

	const esp_timer_create_args_t tick_args = {
		.callback = tick_cb,
		.name     = "lvgl_tick",
	};
	esp_timer_handle_t tick;
	ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer failed");
	ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG,
	                    "tick start failed");

	build_ui();

	BaseType_t ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, 4, NULL);
	ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "could not start the LVGL task");

	ESP_LOGI(TAG, "UI ready");
	return ESP_OK;
}

void app_ui_request_resolution(uint32_t w, uint32_t h)
{
	s_req_w = w;
	s_req_h = h;
}

bool app_ui_take_resolution_request(uint32_t *w, uint32_t *h)
{
	if (!s_req_w || !s_req_h) {
		return false;
	}
	*w = s_req_w;
	*h = s_req_h;
	s_req_w = 0;
	s_req_h = 0;
	return true;
}

void app_ui_request_save(void)
{
	s_req_save = true;
}

bool app_ui_take_save_request(void)
{
	if (!s_req_save) {
		return false;
	}
	s_req_save = false;
	return true;
}

void app_ui_set_status(const char *text, bool ok)
{
	if (!s_status_label || !app_ui_lock(50)) {
		return;
	}
	lv_label_set_text(s_status_label, text);
	lv_obj_set_style_text_color(s_status_label,
	                            ok ? lv_palette_main(LV_PALETTE_GREEN)
	                               : lv_palette_main(LV_PALETTE_RED), 0);
	app_ui_unlock();
}

void app_ui_set_fps(float fps)
{
	if (!s_fps_label || !app_ui_lock(50)) {
		return;
	}
	/*
	 * Formatted with integers on purpose. LVGL's label formatting goes
	 * through picolibc's vsnprintf, which is the integer-only variant here,
	 * so "%.1f" renders as nothing at all. ESP_LOG has its own formatter and
	 * is not affected, which is why the log showed a rate but the label
	 * stayed empty.
	 */
	if (fps < 0.0f) {
		fps = 0.0f;
	}
	int whole = (int)fps;
	int tenth = (int)((fps - (float)whole) * 10.0f + 0.5f);
	if (tenth > 9) {
		whole++;
		tenth = 0;
	}
	lv_label_set_text_fmt(s_fps_label, "%d.%d fps", whole, tenth);
	app_ui_unlock();
}

void app_ui_set_luma(int32_t luma)
{
	if (!s_luma_label || !app_ui_lock(50)) {
		return;
	}
	lv_label_set_text_fmt(s_luma_label, "luma %d", (int)luma);
	app_ui_unlock();
}
