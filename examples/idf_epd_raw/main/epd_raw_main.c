/*
 * epdInky ESP32-P4 - raw E Ink panel demo using the `epd` component
 *
 * The panel here is bare glass: source and gate driver ICs and nothing else.
 * There is no timing controller, so the ESP32-P4 is the timing controller -
 * every clock edge that reaches the glass comes out of the LCD i80 peripheral
 * and a handful of bit-banged control lines.
 *
 * The application's job is small:
 *
 *   1. bring up I2C and the TPS65185, which supplies the panel rails
 *   2. describe how the panel connector is wired to this board
 *   3. pick a panel definition and correct its VCOM for the glass fitted
 *   4. draw into a 4bpp framebuffer and refresh
 *
 * Everything else - waveforms, gate scanning, row DMA, temperature
 * compensation - belongs to the component.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "epd_display.h"
#include "epd_fb.h"
#include "epd_panel.h"
#include "epd_panels.h"

#include "converted_image.h"

static const char *TAG = "epd_raw";

/*
 * How the panel connector is wired to this board.
 *
 * Deliberately separate from the panel definition: swap the glass and these
 * pins are unchanged, move the same glass to another board and they all differ.
 * Every entry comes from the BSP pin map, so there are no magic numbers here.
 *
 * dc_dummy is the one pin with no counterpart on the panel. An EPD has no
 * command/data line, but the i80 peripheral demands a valid DC GPIO and drives
 * it continuously through the GPIO matrix. It idles high and never toggles, so
 * it only has to point somewhere harmless.
 *
 * On this board that is GPIO 36, which the schematic shows going to R50 - a
 * 10K pull-up to 3V3 - and nothing else. The pull-up agrees with the pin's
 * idle-high state, so there is no contention at all.
 *
 * GPIO 36 is a strapping pin (boot-mode select 2 / ROM-print control), which
 * is acceptable here but worth knowing: strapping is latched at reset and the
 * pin is then free for normal use, and it must be HIGH at reset to enter the
 * serial bootloader reliably. R50 holds it high and the DC signal idles high,
 * so both the reset state and the running state are the safe one. Do not
 * reuse this pin for anything that drives it low.
 *
 * The pins that look free but are not:
 *
 *   GPIO 24 / 25   USB_DN / USB_DP - the internal USB Serial/JTAG PHY.
 *                  Configuring GPIO 24 switches the pad away from the USB PHY
 *                  and tears down D- the instant the i80 bus is created: the
 *                  board drops off USB mid-boot and the log stops dead at the
 *                  epd_display pin summary.
 *   GPIO 0 / 1     XTAL_32K_N / XTAL_32K_P, wired to the 32 kHz crystal X1.
 *   GPIO 38        BSP_TPS65185_PIN_INT, the PMIC's active-low interrupt.
 */
static const epd_board_config_t s_board = {
	.data           = BSP_EPD_DATA_PINS_DEFAULT,
	.cl             = BSP_EPD_PIN_XCL,
	.le             = BSP_EPD_PIN_XLE,
	.oe             = BSP_EPD_PIN_XOE,
	.sph            = BSP_EPD_PIN_XSTL,
	.spv            = BSP_EPD_PIN_SPV,
	.ckv            = BSP_EPD_PIN_CKV,
	.gmod           = BSP_EPD_PIN_MODE,
	.dc_dummy       = GPIO_NUM_36,
	.oe_active_high = true,
};

/* Grey levels, for readability: 0x0 is black and 0xF is white. */
#define BLACK 0x0
#define WHITE 0xF

/*
 * Thumbnails used to be a fixed 260x260, sized by eye for ED103TC2's
 * 1872x1404 - see the thumbnail block further down for how their size and
 * placement are now derived from the panel instead.
 */

/*
 * Full black/white cycles run at startup to clear ghosting.
 *
 * Three to five clears most of it; ten is a thorough scrub. Each cycle costs
 * about one INIT, so this is a startup or on-demand operation.
 */
#define CLEAN_CYCLES 1

/*
 * A 16-step greyscale wedge.
 *
 * This is the point of a GC16 waveform, and the quickest way to see whether
 * VCOM and the waveform are right: the steps should be distinct and evenly
 * spaced. If the dark end collapses into a single black, or the light end
 * washes out, suspect VCOM before anything else.
 */
static void draw_grey_wedge(epd_fb_t *fb, int x, int y, int w, int h)
{
	int step = w / 16;

	for (int i = 0; i < 16; i++) {
		epd_fb_fill_rect(fb, x + i * step, y, step, h, (uint8_t)i);
	}

	/* Outline, so the extremes are visible against the page. */
	epd_fb_fill_rect(fb, x, y - 2, step * 16, 2, BLACK);
	epd_fb_fill_rect(fb, x, y + h, step * 16, 2, BLACK);
}

static void draw_page(epd_fb_t *fb, const epd_panel_def_t *def)
{
	char line[96];

	epd_fb_fill(fb, WHITE);

	epd_fb_draw_string(fb, 48, 48, "epdInky ESP32-P4  -  raw EPD driver", 4,
	                   BLACK, WHITE);

	snprintf(line, sizeof(line), "%s   %ux%u   %u-bit bus   VCOM -%u.%02u V",
	         def->name, def->width, def->height, def->bus_width,
	         def->vcom_mv / 1000, (def->vcom_mv % 1000) / 10);
	epd_fb_draw_string(fb, 48, 112, line, 2, BLACK, WHITE);

	epd_fb_fill_rect(fb, 48, 150, fb->width - 96, 3, BLACK);

	epd_fb_draw_string(fb, 48, 190, "16 grey levels (GC16)", 3, BLACK, WHITE);
	draw_grey_wedge(fb, 48, 240, fb->width - 96, 180);

	/* Geometry, to confirm the coordinate system and that nothing is
	 * cropped or offset. */
	epd_fb_draw_string(fb, 48, 470, "Geometry", 3, BLACK, WHITE);
	epd_fb_fill_rect(fb, 48, 520, 320, 220, BLACK);
	epd_fb_fill_rect(fb, 68, 540, 280, 180, WHITE);
	epd_fb_fill_rect(fb, 408, 520, 320, 220, 0x8);
	epd_fb_fill_rect(fb, 768, 520, 320, 220, 0x4);

	/* Single-pixel rules, to show the panel resolves individual rows. */
	for (int i = 0; i < 10; i++) {
		epd_fb_fill_rect(fb, 1140, 520 + i * 22, 320, 1, BLACK);
	}

	/* Frame the whole panel: any cropping or offset shows up immediately. */
	epd_fb_fill_rect(fb, 0, 0, fb->width, 3, BLACK);
	epd_fb_fill_rect(fb, 0, fb->height - 3, fb->width, 3, BLACK);
	epd_fb_fill_rect(fb, 0, 0, 3, fb->height, BLACK);
	epd_fb_fill_rect(fb, fb->width - 3, 0, 3, fb->height, BLACK);

	epd_fb_draw_string(fb, 48, fb->height - 80,
	                   "The border should be complete and the corners square.",
	                   2, BLACK, WHITE);
}

void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4 raw EPD demo");

	/*
	 * Bring up I2C and the PMIC, and nothing else.
	 *
	 * use_epd_gpio is off on purpose. That BSP helper parks the EPD pins as
	 * plain GPIO outputs, but the epd component configures the same pins
	 * itself - the data lines and CL become the i80 peripheral's, the rest
	 * are driven directly. Leaving it on would mean two owners for one set
	 * of pins, and the component is the one that has to win.
	 */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tps65185 = true;    /* required: the panel rails */
	cfg.enable.use_epd_gpio = false;   /* the epd component owns these pins */
	cfg.enable.use_tca6408  = false;
	cfg.enable.use_kxtj3    = false;
	cfg.enable.use_rv3028   = false;
	cfg.enable.use_stc3115  = false;
	cfg.enable.use_sdcard   = false;

	bsp_epdinky_handles_t board;
	ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));

	/*
	 * Take the catalogue entry and correct VCOM for the glass fitted.
	 *
	 * Copied rather than used directly because the catalogue entries are
	 * static constants, and everything else in the definition - geometry,
	 * drive codes, AC timing, the waveform model - is a property of the
	 * model and is already right.
	 */
	epd_panel_def_t panel_def = epd_panel_eink_ed103tc2;

	epd_panel_handle_t panel = NULL;
	ESP_ERROR_CHECK(epd_display_panel_create(&panel_def, &s_board,
	                                         board.tps65185, &panel));

	ESP_LOGI(TAG, "Panel ready: %s %ux%u, %u-bit bus, VCOM -%u mV",
	         panel_def.name, panel_def.width, panel_def.height,
	         panel_def.bus_width, panel_def.vcom_mv);

	epd_fb_t fb;
	ESP_ERROR_CHECK(epd_fb_create(&fb, panel_def.width, panel_def.height));

	draw_page(&fb, &panel_def);

	ESP_ERROR_CHECK(epd_panel_power_on(panel));

	/*
	 * Deep clean first.
	 *
	 * A single INIT is enough before an ordinary update, but a panel that has
	 * been sitting on one image - or updated many times without a full clear -
	 * holds ghosting that one cycle will not shift. Several full black/white
	 * cycles do, at roughly a second each.
	 *
	 * This is a startup and on-demand operation, not something to run before
	 * every frame.
	 */
	ESP_LOGI(TAG, "Deep clean: %d cycles...", CLEAN_CYCLES);
	int64_t t0 = esp_timer_get_time();
	ESP_ERROR_CHECK(epd_panel_clean(panel, CLEAN_CYCLES));
	ESP_LOGI(TAG, "Deep clean done in %lld ms",
	         (esp_timer_get_time() - t0) / 1000);

	/*
	 * INIT before GC16 is not optional.
	 *
	 * GC16 phases can only ever darken a pixel, so the greyscale model
	 * assumes it is starting from a white panel. Without the INIT pass the
	 * image lands on top of whatever the panel happened to be holding -
	 * including from before the board was last powered - and ghosts.
	 */
	t0 = esp_timer_get_time();
	ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT));
	int64_t t_init = esp_timer_get_time() - t0;

	t0 = esp_timer_get_time();
	ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16));
	int64_t t_gc16 = esp_timer_get_time() - t0;

	ESP_LOGI(TAG, "INIT %lld ms, GC16 %lld ms", t_init / 1000, t_gc16 / 1000);

	/*
	 * Power off to hold the test page, rather than just leaving the rails up
	 * and waiting.
	 *
	 * The panel is bistable - it holds its image with no power at all - so
	 * powering down between steps is what actually proves a step finished
	 * correctly: what's on the glass right now cannot be mid-refresh or about
	 * to change, because nothing is driving it. Holding it powered ON for a
	 * few seconds instead would prove nothing extra; it would look identical
	 * whether the image were correct or the panel were about to glitch.
	 *
	 * epd_panel_power_on()/power_off() are cheap to call repeatedly - the PMIC
	 * rail sequencing and discharge steps they run are exactly what every
	 * panel here already goes through once per demo; this just does it three
	 * times instead of one, between steps instead of only at the end.
	 */
	ESP_LOGI(TAG, "Test page shown - powering off to hold it (confirm it looks right with no power)");
	ESP_ERROR_CHECK(epd_panel_power_off(panel));
	vTaskDelay(pdMS_TO_TICKS(5000));

	ESP_LOGI(TAG, "Powering back on for the next step");
	ESP_ERROR_CHECK(epd_panel_power_on(panel));

	/*
	 * Show a photograph next.
	 *
	 * The test page proves geometry and tone; a real image is what shows the
	 * 16 grey levels doing useful work. The asset is 4bpp in the framebuffer's
	 * own packing, but it was generated at ONE specific resolution
	 * (CONVERTED_IMAGE_WIDTH x CONVERTED_IMAGE_HEIGHT, whatever panel it was
	 * made for) - a raw memcpy() only works when that happens to match the
	 * panel actually configured above, and silently corrupts or overflows
	 * otherwise. epd_fb_blit_fit_rot() is the same scale-and-letterbox call
	 * already used for the thumbnails below, applied to the whole panel
	 * instead of a small box: it scales from the source image's own
	 * dimensions to fb's, so this line does not need to change when
	 * panel_def above does. EPD_ROT_AUTO picks whichever of 0/90 degrees
	 * covers more of the panel, so a landscape source image still fills a
	 * portrait-shaped panel (or vice versa) instead of being letterboxed into
	 * a narrow strip.
	 */
	ESP_LOGI(TAG, "Showing image (%ux%u source, fit to %ux%u panel)",
	         CONVERTED_IMAGE_WIDTH, CONVERTED_IMAGE_HEIGHT, fb.width, fb.height);
	epd_fb_blit_fit_rot(&fb, converted_image, CONVERTED_IMAGE_WIDTH,
	                    CONVERTED_IMAGE_HEIGHT, WHITE, EPD_ROT_AUTO);

	/*
	 * INIT again before the image, for the same reason as the first time:
	 * GC16 phases can only darken, so they assume a white baseline. Going
	 * straight from the test page to the photograph would leave the previous
	 * page's black areas showing through.
	 */
	t0 = esp_timer_get_time();
	ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT));
	ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16));
	ESP_LOGI(TAG, "Image shown in %lld ms",
	         (esp_timer_get_time() - t0) / 1000);

	/* Same reasoning as after the test page: power off to hold this step
	 * statically before moving on, rather than just waiting with the rails
	 * still up. */
	ESP_LOGI(TAG, "Image shown - powering off to hold it (confirm it looks right with no power)");
	ESP_ERROR_CHECK(epd_panel_power_off(panel));
	vTaskDelay(pdMS_TO_TICKS(5000));

	ESP_LOGI(TAG, "Powering back on for the next step");
	ESP_ERROR_CHECK(epd_panel_power_on(panel));

	/*
	 * Thumbnails: the same image drawn small, rotated, and placed by hand.
	 *
	 * There is no single call for "scaled, rotated, at this position", but
	 * two existing ones compose into it. epd_fb_blit_fit_rot() rotates and
	 * scales to fill whatever framebuffer it is given - it works from
	 * fb->width and fb->height, not from the panel - so a small scratch
	 * framebuffer turns it into a thumbnail renderer. epd_fb_blit() then
	 * places that scratch buffer anywhere on the real framebuffer, clipping
	 * at the edges and accepting negative coordinates.
	 *
	 * Both rows below size and space themselves from fb.width/fb.height
	 * instead of fixed pixel offsets sized by eye for ED103TC2's 1872x1404 -
	 * the same reason the "Showing image" step earlier uses
	 * epd_fb_blit_fit_rot() instead of a raw memcpy(). Nothing here needs
	 * editing when panel_def above points at a different panel.
	 */
	{
		const int margin = 48;
		const int gap    = 24;

		epd_fb_fill(&fb, WHITE);
		epd_fb_draw_string(&fb, margin, margin,
		                   "Thumbnails: scaled, rotated, positioned", 4,
		                   BLACK, WHITE);

		int y = margin + 62;   /* clears the scale-4 title text */
		epd_fb_fill_rect(&fb, margin, y, fb.width - 2 * margin, 3, BLACK);
		y += 30;

		/* Two thumbnail rows (each a row of thumbnails plus its
		 * label/caption line) split whatever height is left evenly. */
		const int label_h = 34;
		const int row_h   = ((int)fb.height - y - margin) / 2;

		/* Row 1: the four fixed rotations, evenly spaced to fill the width. */
		static const struct {
			epd_rotation_t rot;
			const char    *label;
		} row1[] = {
			{ EPD_ROT_0,   "ROT 0"   },
			{ EPD_ROT_90,  "ROT 90"  },
			{ EPD_ROT_180, "ROT 180" },
			{ EPD_ROT_270, "ROT 270" },
		};
		const int row1_count = sizeof(row1) / sizeof(row1[0]);

		/* Square thumbnails (so 90/270 rotation is obvious rather than just
		 * re-letterboxed), sized by whichever axis is tighter: enough
		 * columns to fill the width, or the height budget left for a row
		 * plus its label. Width must stay even - two 4bpp pixels share a
		 * byte. */
		int thumb1 = ((int)fb.width - 2 * margin - (row1_count - 1) * gap) / row1_count;
		int thumb1_h_budget = row_h - label_h;
		if (thumb1 > thumb1_h_budget) {
			thumb1 = thumb1_h_budget;
		}
		thumb1 &= ~1;

		epd_fb_t thumb;
		ESP_ERROR_CHECK(epd_fb_create(&thumb, (uint16_t)thumb1, (uint16_t)thumb1));

		for (int i = 0; i < row1_count; i++) {
			int x = margin + i * (thumb1 + gap);

			epd_fb_blit_fit_rot(&thumb, converted_image,
			                    CONVERTED_IMAGE_WIDTH, CONVERTED_IMAGE_HEIGHT,
			                    WHITE, row1[i].rot);
			epd_fb_blit(&fb, x, y, thumb.buf, (uint16_t)thumb1, (uint16_t)thumb1);

			/* Outline, so the thumbnail's extent is visible even where the
			 * image itself is white. */
			epd_fb_fill_rect(&fb, x - 2, y - 2, thumb1 + 4, 2, BLACK);
			epd_fb_fill_rect(&fb, x - 2, y + thumb1, thumb1 + 4, 2, BLACK);
			epd_fb_fill_rect(&fb, x - 2, y - 2, 2, thumb1 + 4, BLACK);
			epd_fb_fill_rect(&fb, x + thumb1, y - 2, 2, thumb1 + 4, BLACK);

			epd_fb_draw_string(&fb, x, y + thumb1 + 12, row1[i].label, 3,
			                   BLACK, WHITE);

			/* Scaling a full-panel image down to a thumbnail box-averages
			 * many source pixels per output pixel, which is enough work
			 * with no natural yield that back-to-back calls starve the
			 * idle task and trip the task watchdog, so give it a moment
			 * between thumbnails. */
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		epd_fb_destroy(&thumb);

		/* Row 2: arbitrary positions, including one deliberately clipped off
		 * the right edge to show that blitting is bounds-safe. Spread across
		 * whatever width is left rather than fixed offsets; the clipped one
		 * is always placed half off the right edge, wherever that edge is. */
		int y2_caption = y + row_h;
		epd_fb_draw_string(&fb, margin, y2_caption,
		                   "Arbitrary positions (last one clipped at the edge)",
		                   3, BLACK, WHITE);
		int y2 = y2_caption + label_h;

		int thumb2 = (int)fb.height - y2 - margin;
		if (thumb2 > thumb1) {
			thumb2 = thumb1;   /* no reason for row 2 to run bigger than row 1 */
		}
		if (thumb2 < 40) {
			thumb2 = 40;   /* floor so a very short panel still shows something recognisable */
		}
		thumb2 &= ~1;

		ESP_ERROR_CHECK(epd_fb_create(&thumb, (uint16_t)thumb2, (uint16_t)thumb2));

		static const epd_rotation_t row2_rot[] = {
			EPD_ROT_0, EPD_ROT_90, EPD_ROT_180, EPD_ROT_AUTO,
		};
		const int row2_count = sizeof(row2_rot) / sizeof(row2_rot[0]);

		for (int i = 0; i < row2_count; i++) {
			/* Evenly spread from the left margin to the right margin. */
			int x = margin + i * ((int)fb.width - 2 * margin - thumb2) / (row2_count - 1);

			epd_fb_blit_fit_rot(&thumb, converted_image,
			                    CONVERTED_IMAGE_WIDTH, CONVERTED_IMAGE_HEIGHT,
			                    WHITE, row2_rot[i]);
			epd_fb_blit(&fb, x, y2, thumb.buf, (uint16_t)thumb2, (uint16_t)thumb2);

			epd_fb_fill_rect(&fb, x - 2, y2 - 2, thumb2 + 4, 2, BLACK);
			epd_fb_fill_rect(&fb, x - 2, y2 + thumb2, thumb2 + 4, 2, BLACK);

			vTaskDelay(pdMS_TO_TICKS(10));
		}

		/* The deliberately clipped one - half off the right edge, whatever
		 * that edge is. */
		{
			int x = (int)fb.width - thumb2 / 2;

			epd_fb_blit_fit_rot(&thumb, converted_image,
			                    CONVERTED_IMAGE_WIDTH, CONVERTED_IMAGE_HEIGHT,
			                    WHITE, EPD_ROT_0);
			epd_fb_blit(&fb, x, y2, thumb.buf, (uint16_t)thumb2, (uint16_t)thumb2);

			epd_fb_fill_rect(&fb, x - 2, y2 - 2, thumb2 + 4, 2, BLACK);
			epd_fb_fill_rect(&fb, x - 2, y2 + thumb2, thumb2 + 4, 2, BLACK);
		}

		epd_fb_destroy(&thumb);

		t0 = esp_timer_get_time();
		ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT));
		ESP_ERROR_CHECK(epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16));
		ESP_LOGI(TAG, "Thumbnails shown in %lld ms",
		         (esp_timer_get_time() - t0) / 1000);
	}

	/*
	 * Drop the rails a third and final time, to hold the thumbnails step the
	 * same way the previous two steps were held - see the comment after the
	 * test page above for why powering off (rather than just waiting with the
	 * rails up) is what actually confirms a step finished correctly.
	 *
	 * Not just to save energy: with the rails up, VCOM sits live at its
	 * operating voltage while the source lines rest near ground, so every
	 * pixel is held under a DC field of the full VCOM magnitude. Left that
	 * way the pigment drifts and white areas turn visibly grainy within
	 * minutes. Powering down parks the panel neutral.
	 *
	 * This leaves the PMIC in STANDBY rather than sleep: dropping its WAKEUP
	 * line powers down the I2C interface entirely, which is a system-level
	 * decision the driver should not make on its own.
	 */
	ESP_ERROR_CHECK(epd_panel_power_off(panel));

	epd_fb_destroy(&fb);

	ESP_LOGI(TAG, "Done - the image is retained with the panel powered down");

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}
