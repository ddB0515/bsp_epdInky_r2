/*
 * epdInky ESP32-P4 - FastEPD parallel e-paper example
 *
 * FastEPD owns the e-paper side of the board completely: the 16-bit parallel
 * bus, the row and gate timing, the greyscale waveforms and the TPS65185 PMIC.
 * The application only brings up the I2C master and says which panel is
 * fitted.
 *
 *
 * How the I2C bus is shared
 * -------------------------
 * FastEPD needs I2C for the PMIC, but it does not insist on owning the bus.
 * Its bbepI2CInit() first calls i2c_master_get_bus_handle(I2C_NUM_0) and
 * reuses whatever is already there, creating a bus only if that fails. The BSP
 * creates its bus on I2C_NUM_0 with the same pins, so bringing the board up
 * first means FastEPD simply adopts it and both can share it.
 *
 *
 * Which panel definition
 * ----------------------
 * FastEPD already ships a definition for this board: BB_PANEL_EPDINKY_P4_16.
 * Its pin map matches the schematic exactly - PWRUP 26, SPV 45, CKV 51,
 * XSTL 46, XOE 47, XLE 48, XCL 50, PWR_GOOD 27, WAKEUP 37, VCOM_CTRL 49,
 * MODE 52, SDA 28, SCL 29 - so no pins have to be described by hand.
 *
 * The 16-bit variant is used because this board wires all of D0..D15. There is
 * also an 8-bit BB_PANEL_EPDINKY_P4, which is slower.
 *
 * That definition deliberately leaves width and height at zero, because the
 * board is a carrier and the panel varies. bbepInitPanel() skips its buffer
 * allocation when the size is zero, so setPanelSize() must follow initPanel():
 * that call is what allocates the frame buffers and builds the greyscale
 * lookup tables.
 *
 *
 * VCOM
 * ----
 * VCOM belongs to the glass, not the board, and the wrong value gives a washed
 * out image or heavy ghosting. The value below is for the ED103TC2 fitted
 * here and is printed on the panel's own flexible cable. FastEPD writes it to
 * the TPS65185 as iVCOM / -10, so -1250 becomes 125 in the PMIC's 10 mV steps.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "bsp/epdinky_p4_board.h"

#include "FastEPD.h"

static const char *TAG = "epd_fastepd";

/*
 * The panel fitted to this board.
 *
 * ED103TC2 is a 10.3" 1872x1404 panel. Size and VCOM both come from the glass,
 * so change them together if a different panel is fitted.
 */
#define EPD_PANEL_NAME "ED103TC2"
#define EPD_WIDTH      1872
#define EPD_HEIGHT     1404
#define EPD_VCOM_MV    (-1250)   /* -1.25 V, printed on the panel cable */

/*
 * Panel orientation.
 *
 * Whether the source driver counts left to right or the other way depends on
 * how the glass is bonded and cannot be detected. If the image comes out
 * mirrored horizontally, change this to BB_PANEL_FLAG_MIRROR_X.
 */
#define EPD_FLAGS BB_PANEL_FLAG_MIRROR_Y

static FASTEPD epaper;

static const char *epd_err(int rc)
{
	switch (rc) {
	case BBEP_SUCCESS:             return "success";
	case BBEP_ERROR_BAD_PARAMETER: return "bad parameter";
	case BBEP_ERROR_BAD_DATA:      return "bad data";
	case BBEP_ERROR_NOT_SUPPORTED: return "not supported";
	case BBEP_ERROR_NO_MEMORY:     return "out of memory";
	case BBEP_ERROR_OUT_OF_BOUNDS: return "out of bounds";
	case BBEP_IO_ERROR:            return "I/O error";
	default:                       return "unknown";
	}
}

/*
 * A dithered density ramp.
 *
 * The panel is driven in 1-bit mode, so there are no grey levels to show. An
 * ordered dither fakes them instead: each block turns on a fixed fraction of
 * its pixels using a 4x4 Bayer threshold, which the eye averages into an
 * apparent shade. This is how to get tone out of a 1bpp panel.
 */
static void draw_dither_ramp(int x, int y, int w, int h)
{
	static const uint8_t bayer[16] = {
		 0,  8,  2, 10,
		12,  4, 14,  6,
		 3, 11,  1,  9,
		15,  7, 13,  5,
	};

	const int steps = 16;
	int       step  = w / steps;

	for (int i = 0; i < steps; i++) {
		int bx = x + i * step;
		for (int py = 0; py < h; py++) {
			for (int px = 0; px < step; px++) {
				/* i = 0 leaves the block white, i = 15 fills it black. */
				bool on = bayer[(py & 3) * 4 + (px & 3)] < i;
				if (on) {
					epaper.drawPixel(bx + px, y + py, BBEP_BLACK);
				}
			}
		}
	}
	epaper.drawRect(x, y, step * steps, h, BBEP_BLACK);
}

static void draw_demo(void)
{
	/*
	 * 1-bit mode.
	 *
	 * This board cannot drive the fitted panel in 4bpp - the greyscale
	 * waveform produces nothing on the glass - so everything here is pure
	 * black and white, with dithering standing in for tone.
	 */
	epaper.setMode(BB_MODE_1BPP);
	epaper.fillScreen(BBEP_WHITE);
	epaper.setTextColor(BBEP_BLACK, BBEP_WHITE);

	epaper.setFont(FONT_12x16);
	epaper.setCursor(48, 56);
	epaper.print("epdInky ESP32-P4  -  FastEPD");

	epaper.setFont(FONT_8x8);
	epaper.setCursor(48, 96);
	epaper.print(EPD_PANEL_NAME "  1872x1404  VCOM -1.25V  16-bit bus  1bpp");

	epaper.drawLine(48, 118, EPD_WIDTH - 48, 118, BBEP_BLACK);

	epaper.setFont(FONT_12x16);
	epaper.setCursor(48, 158);
	epaper.print("Dithered tone ramp (1-bit)");
	draw_dither_ramp(48, 190, EPD_WIDTH - 96, 170);

	/* Plain geometry, to confirm the coordinate system and the aspect. */
	epaper.setCursor(48, 420);
	epaper.print("Geometry");
	epaper.drawRect(48, 452, 300, 200, BBEP_BLACK);
	epaper.fillRect(388, 452, 300, 200, BBEP_BLACK);
	epaper.drawLine(48, 452, 348, 652, BBEP_BLACK);
	epaper.drawLine(48, 652, 348, 452, BBEP_BLACK);

	/* A few horizontal rules at 1px, to show the panel resolves single rows. */
	for (int i = 0; i < 8; i++) {
		epaper.drawLine(728, 452 + i * 24, 1028, 452 + i * 24, BBEP_BLACK);
	}

	/* Frame the whole panel, so any cropping or offset is obvious. */
	epaper.drawRect(0, 0, EPD_WIDTH, EPD_HEIGHT, BBEP_BLACK);
	epaper.drawRect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, BBEP_BLACK);

	epaper.setFont(FONT_8x8);
	epaper.setCursor(48, EPD_HEIGHT - 64);
	epaper.print("The border should be complete and the corners square.");
}

extern "C" void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4 FastEPD example");

	/*
	 * Bring up the I2C master only.
	 *
	 * The TPS65185 driver is deliberately left out: FastEPD drives the PMIC
	 * itself, and two owners fighting over the power sequence is a good way
	 * to damage a panel. The EPD GPIO block is left out for the same reason,
	 * as FastEPD configures those pins in its own IO init.
	 */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tps65185 = false;
	cfg.enable.use_epd_gpio = false;
	cfg.enable.use_tca6408  = false;
	cfg.enable.use_kxtj3    = false;
	cfg.enable.use_rv3028   = false;
	cfg.enable.use_stc3115  = false;
	cfg.enable.use_sdcard   = false;

	bsp_epdinky_handles_t board;
	ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));

	/* The PMIC should answer at 0x68 before FastEPD tries to use it. */
	bsp_i2c_scan();

	int rc = epaper.initPanel(BB_PANEL_EPDINKY_P4_16);
	if (rc != BBEP_SUCCESS) {
		ESP_LOGE(TAG, "initPanel failed: %s", epd_err(rc));
		return;
	}

	/* The board definition carries no panel size, so it is supplied here
	 * together with the VCOM the fitted glass needs. This call is also what
	 * allocates the frame buffers. */
	rc = epaper.setPanelSize(EPD_WIDTH, EPD_HEIGHT, EPD_FLAGS, EPD_VCOM_MV);
	if (rc != BBEP_SUCCESS) {
		ESP_LOGE(TAG, "setPanelSize failed: %s", epd_err(rc));
		return;
	}

	ESP_LOGI(TAG, "Panel ready: %s %dx%d, VCOM %d mV",
	         EPD_PANEL_NAME, epaper.width(), epaper.height(), EPD_VCOM_MV);

	/*
	 * Clear to white first.
	 *
	 * E-paper holds whatever it was last showing, including from before a
	 * power cycle, and drawing on top of an unknown image leaves ghosting.
	 * clearWhite() drives every pixel to a known state.
	 */
	ESP_LOGI(TAG, "Clearing to white...");
	rc = epaper.clearWhite(true);   /* keep the rails up for the update below */
	if (rc != BBEP_SUCCESS) {
		ESP_LOGE(TAG, "clearWhite failed: %s", epd_err(rc));
		ESP_LOGE(TAG, "An I/O error here is usually the panel not being "
		              "connected, or PWR_GOOD never arriving from the PMIC.");
		return;
	}

	ESP_LOGI(TAG, "Drawing...");
	draw_demo();

	ESP_LOGI(TAG, "Updating...");
	rc = epaper.fullUpdate(CLEAR_SLOW, false);
	if (rc != BBEP_SUCCESS) {
		ESP_LOGE(TAG, "fullUpdate failed: %s", epd_err(rc));
		return;
	}

	/* Drop the rails. E-paper keeps its image with no power at all, and
	 * leaving the high voltage up shortens the panel's life. */
	epaper.einkPower(0);
	ESP_LOGI(TAG, "Done - the image stays with the panel powered down");

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}
