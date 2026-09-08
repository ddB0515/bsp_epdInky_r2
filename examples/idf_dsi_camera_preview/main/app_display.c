/*
 * MIPI-DSI panel bring-up for the epdInky ESP32-P4 board.
 *
 * The panel is a D320C2403V1-MIPI: 3.2", 1024x768, JD9168 driver, GT967
 * capacitive touch, with an SGM37604A backlight controller on the module.
 *
 * The panel sits on an adapter board carrying a second TCA6408 at 0x20 (the
 * mainboard has one at 0x21). That expander gates everything:
 *
 *   P0  GPIO_EN  -> AP2281 load switch -> LCD_VDD
 *   P1  LCD_RES  -> JD9168 reset, active low
 *   P2  BL_EN    -> SGM37604A hardware enable
 *
 * So the order matters. The backlight controller does not acknowledge on I2C
 * until P2 is high, and the panel has no power until P0 is high - before that
 * a bus scan shows neither 0x36 nor 0x5D and looks exactly like an unplugged
 * display. Panel reset is a register write here, not a GPIO, which is why the
 * JD9168 is created with reset_gpio_num = -1.
 *
 * The frame buffer is RGB888, matching the configuration the vendor validated
 * this panel with. RGB565 was tried first because it halves both the memory and
 * the DSI bandwidth, but the panel showed nothing at all - not even the DSI
 * controller's own colour bars - so the JD9168 init sequence evidently expects
 * 24-bit pixels.
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "esp_lcd_jd9168.h"
#include "sgm37604a.h"
#include "tca6408.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "app_display.h"

static const char *TAG = "app_display";

static tca6408_handle_t          s_exp;      /* adapter-board expander, 0x20 */
static esp_lcd_dsi_bus_handle_t  s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t    s_panel;
static void                     *s_fb;
static SemaphoreHandle_t         s_trans_done;
static volatile uint32_t         s_vsync_count;

esp_err_t app_display_init(void)
{
	ESP_RETURN_ON_FALSE(!s_panel, ESP_ERR_INVALID_STATE, TAG, "already initialised");

	/* The adapter shares the board I2C bus with the sensors and the camera. */
	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "I2C bus unavailable");

	ESP_RETURN_ON_ERROR(tca6408_init(bus, BSP_I2C_ADDR_LCD_EXPANDER, &s_exp), TAG,
	                    "no expander at 0x%02X - is the display adapter fitted?",
	                    BSP_I2C_ADDR_LCD_EXPANDER);

	/* Everything starts low and off: panel unpowered, held in reset, backlight
	 * disabled. Then make P0..P2 outputs (0 = output on a TCA6408). */
	ESP_RETURN_ON_ERROR(tca6408_set_output_val(s_exp, 0x00), TAG, "expander preset failed");
	ESP_RETURN_ON_ERROR(tca6408_set_config(s_exp, (uint8_t)~((1 << BSP_LCD_EXP_PIN_POWER_EN) |
	                                                         (1 << BSP_LCD_EXP_PIN_PANEL_RST) |
	                                                         (1 << BSP_LCD_EXP_PIN_BACKLIGHT_EN))),
	                    TAG, "expander direction failed");

	/* Power the panel, let its rails settle, then release reset. */
	ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_POWER_EN, 1), TAG,
	                    "panel power on failed");
	vTaskDelay(pdMS_TO_TICKS(20));

	ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_PANEL_RST, 0), TAG,
	                    "panel reset assert failed");
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_PANEL_RST, 1), TAG,
	                    "panel reset release failed");
	vTaskDelay(pdMS_TO_TICKS(120));   /* JD9168 needs time before it accepts commands */

	/* Only now will the backlight controller answer on I2C. */
	ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_BACKLIGHT_EN, 1), TAG,
	                    "backlight enable failed");
	vTaskDelay(pdMS_TO_TICKS(10));

	ESP_RETURN_ON_ERROR(sgm37604a_init(bus, SGM37604A_CURRENT_30MA), TAG,
	                    "backlight init failed");

	/* The DSI bus creation also brings up the D-PHY. */
	esp_lcd_dsi_bus_config_t bus_config = {
		.bus_id             = 0,
		.num_data_lanes     = BSP_DSI_DATA_LANES,
		.phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
		.lane_bit_rate_mbps = BSP_DSI_LANE_BITRATE_MBPS,
	};
	ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus), TAG,
	                    "could not create the DSI bus");

	/* Commands and parameters go over DBI; pixels go over DPI. */
	esp_lcd_dbi_io_config_t dbi_config = {
		.virtual_channel = 0,
		.lcd_cmd_bits    = 8,
		.lcd_param_bits  = 8,
	};
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io), TAG,
	                    "could not create the DBI IO");

	esp_lcd_dpi_panel_config_t dpi_config =
		JD9168_1024_768_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB888);

	jd9168_vendor_config_t vendor_config = {
		.mipi_config = {
			.dsi_bus    = s_dsi_bus,
			.dpi_config = &dpi_config,
		},
	};
	esp_lcd_panel_dev_config_t panel_config = {
		.reset_gpio_num = BSP_DSI_PIN_PANEL_RST,   /* not routed on this board */
		.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = APP_DISPLAY_BPP,
		.vendor_config  = &vendor_config,
	};

	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9168(s_dbi_io, &panel_config, &s_panel), TAG,
	                    "could not create the JD9168 panel");

	/*
	 * No esp_lcd_panel_reset() here.
	 *
	 * The panel has already been hardware-reset through the adapter's IO
	 * expander above. With reset_gpio_num set to NC the driver would instead
	 * send a DCS software reset over DBI, which puts the JD9168 back to its
	 * power-on defaults right before the init sequence runs.
	 */
	ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

	ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_fb), TAG,
	                    "could not get the frame buffer");

	/* Clear to black, so nothing stale is shown when the backlight comes up. */
	memset(s_fb, 0, app_display_frame_buffer_size());
	app_display_sync_cache(s_fb, app_display_frame_buffer_size());

	uint8_t faults = 0;
	if (sgm37604a_get_faults(&faults) == ESP_OK) {
		if (faults) {
			ESP_LOGW(TAG, "Backlight fault flags 0x%02X - LED string open or shorted?",
			         faults);
		} else {
			ESP_LOGI(TAG, "Backlight reports no faults");
		}
	}

	ESP_LOGI(TAG, "Panel ready: %dx%d RGB%s, %d DSI lanes at %d Mbps",
	         BSP_DSI_LCD_H_RES, BSP_DSI_LCD_V_RES,
	         (APP_DISPLAY_BPP == 24) ? "888" : "565",
	         BSP_DSI_DATA_LANES, BSP_DSI_LANE_BITRATE_MBPS);
	return ESP_OK;
}

esp_lcd_panel_handle_t app_display_panel(void)
{
	return s_panel;
}

void *app_display_frame_buffer(void)
{
	return s_fb;
}

size_t app_display_frame_buffer_size(void)
{
	return (size_t)BSP_DSI_LCD_H_RES * BSP_DSI_LCD_V_RES * APP_DISPLAY_BYTES_PER_PX;
}

void app_display_fill(uint8_t r, uint8_t g, uint8_t b)
{
	if (!s_fb) {
		return;
	}
	uint8_t *px = (uint8_t *)s_fb;
	size_t   n  = (size_t)BSP_DSI_LCD_H_RES * BSP_DSI_LCD_V_RES;
	for (size_t i = 0; i < n; i++) {
		px[i * 3 + 0] = b;
		px[i * 3 + 1] = g;
		px[i * 3 + 2] = r;
	}
	app_display_sync_cache(s_fb, app_display_frame_buffer_size());
}

void app_display_sync_cache(void *addr, size_t size){
	if (!addr || !size) {
		return;
	}
	/* The DPI frame buffer lives in PSRAM and is scanned out by DMA. CPU
	 * writes stop in the cache, so without this write-back the panel shows
	 * whatever happened to be evicted - typically streaks of stale pixels.
	 * esp_lcd_panel_draw_bitmap() does this internally, which is why the
	 * LVGL path works and direct writes need it done by hand. */
	esp_err_t err = esp_cache_msync(addr, size,
	                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
	                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "cache write-back failed: %s", esp_err_to_name(err));
	}
}

void app_display_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
	if (!s_fb) {
		return;
	}
	if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
	    x + w > BSP_DSI_LCD_H_RES || y + h > BSP_DSI_LCD_V_RES) {
		return;
	}

	uint8_t *fb     = (uint8_t *)s_fb;
	size_t   stride = (size_t)BSP_DSI_LCD_H_RES * APP_DISPLAY_BYTES_PER_PX;

	for (int row = y; row < y + h; row++) {
		uint8_t *px = fb + (size_t)row * stride + (size_t)x * APP_DISPLAY_BYTES_PER_PX;
		for (int col = 0; col < w; col++) {
			px[col * 3 + 0] = b;
			px[col * 3 + 1] = g;
			px[col * 3 + 2] = r;
		}
	}

	/* Sync whole rows covering the rectangle: cache lines are wider than a
	 * pixel, so syncing a sub-row range would leave ragged edges. */
	app_display_sync_cache(fb + (size_t)y * stride, (size_t)h * stride);
}

static bool on_trans_done(esp_lcd_panel_handle_t panel,
                          esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
	(void)panel;
	(void)edata;
	BaseType_t hp = pdFALSE;
	if (s_trans_done) {
		xSemaphoreGiveFromISR(s_trans_done, &hp);
	}
	return hp == pdTRUE;
}

static bool on_refresh_done(esp_lcd_panel_handle_t panel,
                            esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
	(void)panel;
	(void)edata;
	(void)user_ctx;
	s_vsync_count++;
	return false;
}

/*
 * Does the DPI data path actually move pixels?
 *
 * The DSI colour-bar pattern is generated inside the host and bypasses the
 * DMA entirely, so it can look perfect while frame buffer scan-out is dead.
 * This pushes a stripe through esp_lcd_panel_draw_bitmap() and waits for the
 * completion interrupt, which answers the question without needing anyone to
 * look at the panel.
 */
esp_err_t app_display_selftest(void)
{
	ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "not initialised");

	s_trans_done = xSemaphoreCreateBinary();
	ESP_RETURN_ON_FALSE(s_trans_done, ESP_ERR_NO_MEM, TAG, "no memory for the semaphore");

	esp_lcd_dpi_panel_event_callbacks_t cbs = {
		.on_color_trans_done = on_trans_done,
		.on_refresh_done     = on_refresh_done,
	};
	esp_err_t err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "could not register the DPI callback: %s", esp_err_to_name(err));
		return err;
	}

	const int  stripe_h = 128;
	size_t     bytes    = (size_t)BSP_DSI_LCD_H_RES * stripe_h * APP_DISPLAY_BYTES_PER_PX;
	uint8_t   *stripe   = heap_caps_aligned_calloc(64, 1, bytes,
	                                               MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	ESP_RETURN_ON_FALSE(stripe, ESP_ERR_NO_MEM, TAG, "no memory for the test stripe");

	/* Bright magenta: distinct from anything the panel shows on its own. */
	for (size_t i = 0; i < (size_t)BSP_DSI_LCD_H_RES * stripe_h; i++) {
		stripe[i * 3 + 0] = 255;   /* B */
		stripe[i * 3 + 1] = 0;     /* G */
		stripe[i * 3 + 2] = 255;   /* R */
	}
	esp_cache_msync(stripe, bytes,
	                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

	ESP_LOGI(TAG, "SELFTEST: pushing a magenta stripe via draw_bitmap...");
	err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BSP_DSI_LCD_H_RES, stripe_h, stripe);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "SELFTEST: draw_bitmap returned %s", esp_err_to_name(err));
		free(stripe);
		return err;
	}

	if (xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(1000)) == pdTRUE) {
		ESP_LOGI(TAG, "SELFTEST PASS: the DPI moved the data and signalled completion");
	} else {
		ESP_LOGE(TAG, "SELFTEST FAIL: no completion interrupt - the DPI DMA is stalled");
		ESP_LOGE(TAG, "  The colour bars come from the DSI host and bypass this path,");
		ESP_LOGE(TAG, "  which is why they look fine while the image stays frozen.");
	}

	free(stripe);

	/*
	 * Is the bridge actually scanning the frame buffer out to the panel?
	 *
	 * A VSYNC fires once per refresh, so at 60 Hz roughly 30 are expected in
	 * half a second. Zero means the frame buffer never reaches the panel,
	 * which looks identical to a working link because the DSI colour bars
	 * are generated further downstream.
	 */
	s_vsync_count = 0;
	vTaskDelay(pdMS_TO_TICKS(500));
	uint32_t seen = s_vsync_count;
	if (seen == 0) {
		ESP_LOGE(TAG, "SELFTEST FAIL: no VSYNC in 500 ms - the panel is not being refreshed");
	} else {
		ESP_LOGI(TAG, "SELFTEST: %" PRIu32 " VSYNC in 500 ms (~%" PRIu32 " Hz refresh)",
		         seen, seen * 2);
	}

	return ESP_OK;
}

esp_err_t app_display_set_backlight(uint8_t percent)
{
	return sgm37604a_set_brightness_percent(percent);
}

esp_err_t app_display_test_pattern(bool on)
{
	ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "not initialised");
	return esp_lcd_dpi_panel_set_pattern(s_panel,
	                                     on ? MIPI_DSI_PATTERN_BAR_VERTICAL
	                                        : MIPI_DSI_PATTERN_NONE);
}
