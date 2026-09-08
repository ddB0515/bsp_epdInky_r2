/*
 * epdInky ESP32-P4 - Arduino + the repository's own epd driver
 *
 * The counterpart to the arduino_epd_fastepd example. That one hands the whole
 * panel to FastEPD; this one uses the `epd` component in this repository, which
 * drives the panel directly and gives true 16-level greyscale.
 *
 * Build with ESP-IDF (arduino-esp32 is pulled in as a managed component):
 *   idf.py set-target esp32p4
 *   idf.py build flash monitor
 *
 *
 * Serial, and why it is not UART0
 * -------------------------------
 * On this board UART0 is GPIO37/GPIO38, which are the TPS65185 WAKEUP and INT
 * lines. Arduino's default `Serial` is UART0, so using it would drive the PMIC
 * control lines as a UART. The top-level CMakeLists sets ARDUINO_USB_CDC_ON_BOOT
 * so `Serial` is the USB Serial/JTAG device instead.
 *
 *
 * The BSP is used here
 * --------------------
 * Unlike the FastEPD sketch, which had to skip the BSP because FastEPD's
 * Arduino path calls Wire.begin() and collides with it, the epd component takes
 * the I2C bus handle it is given. So the BSP brings up I2C and the PMIC, and
 * everything else on the board stays available to the sketch.
 *
 *
 * What this driver does that FastEPD does not
 * -------------------------------------------
 * True 16-level greyscale via GC16, rather than 1bpp with dithering. The cost
 * is that a full update is INIT + GC16 - about 1.4 s and a visible flash.
 *
 * It also supports windowed updates with EPD_WAVEFORM_DU: no flash, the rest of
 * the image untouched, and about half the cost of a full refresh. Less than half
 * might be expected, because a DU pass still clocks every gate row - the gate
 * driver is a shift register with no random access - so the saving comes from
 * needing fewer frames, not from touching fewer pixels.
 *
 * DU can only ever darken a pixel: a partial update can add ink, never remove
 * it. Removing anything needs a full refresh.
 *
 * The demo below shows both.
 */

#include <Arduino.h>

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "epd_display.h"
#include "epd_fb.h"
#include "epd_panel.h"
#include "epd_panels.h"

/*
 * How the panel connector is wired to this board.
 *
 * Deliberately separate from the panel definition: swap the glass and these
 * pins are unchanged, move the same glass to another board and they all differ.
 * Every entry comes from the BSP pin map, so there are no magic numbers here.
 *
 * dc_dummy is the one pin with no counterpart on the panel. An EPD has no
 * command/data line, but the i80 peripheral demands a valid DC GPIO and drives
 * it continuously. It idles high and never toggles, so it only has to point
 * somewhere harmless - GPIO 36, which goes to a 10K pull-up and nothing else.
 *
 * The pins that look free but are not:
 *
 *   GPIO 24 / 25   USB_DN / USB_DP - the USB Serial/JTAG PHY. Configuring
 *                  GPIO 24 tears down the USB link the moment the i80 bus is
 *                  created, and the board drops off USB mid-boot.
 *   GPIO 0 / 1     XTAL_32K_N / XTAL_32K_P, wired to the 32 kHz crystal.
 *   GPIO 38        BSP_TPS65185_PIN_INT, the PMIC's active-low interrupt.
 */
static const epd_board_config_t kBoard = {
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

#define BLACK 0x0
#define WHITE 0xF

/*
 * Full black/white cycles run at startup to clear ghosting.
 *
 * A single INIT is enough before an ordinary update, but a panel that has been
 * sitting on one image holds ghosting that one cycle will not shift.
 */
#define CLEAN_CYCLES 3

/* Seconds between full redraws. */
#define UPDATE_INTERVAL_S 60

/*
 * Set to 1 to run the DU drift probe instead of the normal demo.
 *
 * Kept because it is the tool that found the idle-code bias: a uniform shift in
 * brightness is nearly impossible to judge by eye, and the probe makes it
 * self-comparing. See duProbe().
 */
#define DU_DARKENING_PROBE 0

static epd_panel_handle_t panel;
static epd_fb_t           fb;
static uint32_t           lastUpdate;
static uint32_t           updateCount;
static bool               ready;

/*
 * A 16-step greyscale wedge.
 *
 * The quickest way to see whether VCOM is right: the steps should be evenly
 * spaced from black to white. A wrong VCOM compresses them at one end, and
 * because that degrades the image rather than producing an error it is easy to
 * miss without something like this on screen.
 */
static void drawGreyWedge(epd_fb_t *f, int x, int y, int w, int h)
{
	const int step = w / 16;

	for (int i = 0; i < 16; i++) {
		epd_fb_fill_rect(f, x + i * step, y, step, h, (uint8_t)i);
	}

	epd_fb_fill_rect(f, x, y - 2, step * 16, 2, BLACK);
	epd_fb_fill_rect(f, x, y + h, step * 16, 2, BLACK);
}

static void drawPage(epd_fb_t *f, const epd_panel_def_t *def)
{
	char line[96];

	epd_fb_fill(f, WHITE);

	epd_fb_draw_string(f, 48, 48, "epdInky ESP32-P4  -  Arduino + epd driver",
	                   4, BLACK, WHITE);

	snprintf(line, sizeof(line), "%s   %ux%u   %u-bit bus   VCOM -%u.%02u V",
	         def->name, def->width, def->height, def->bus_width,
	         def->vcom_mv / 1000, (def->vcom_mv % 1000) / 10);
	epd_fb_draw_string(f, 48, 112, line, 2, BLACK, WHITE);

	epd_fb_fill_rect(f, 48, 150, f->width - 96, 3, BLACK);

	/* Live values, to show the screen is really being redrawn. */
	epd_fb_draw_string(f, 48, 190, "Status", 3, BLACK, WHITE);

	snprintf(line, sizeof(line), "Update     : %lu", (unsigned long)updateCount);
	epd_fb_draw_string(f, 48, 240, line, 2, BLACK, WHITE);

	snprintf(line, sizeof(line), "Uptime     : %lu s",
	         (unsigned long)(millis() / 1000));
	epd_fb_draw_string(f, 48, 275, line, 2, BLACK, WHITE);

	snprintf(line, sizeof(line), "Free heap  : %lu bytes",
	         (unsigned long)ESP.getFreeHeap());
	epd_fb_draw_string(f, 48, 310, line, 2, BLACK, WHITE);

	snprintf(line, sizeof(line), "Free PSRAM : %lu bytes",
	         (unsigned long)ESP.getFreePsram());
	epd_fb_draw_string(f, 48, 345, line, 2, BLACK, WHITE);

	/* 16 real grey levels - the thing this driver offers over 1bpp. */
	epd_fb_draw_string(f, 48, 410, "16 grey levels (GC16)", 3, BLACK, WHITE);
	drawGreyWedge(f, 48, 460, f->width - 96, 180);

	/* Geometry, to confirm the coordinate system and that nothing is cropped
	 * or offset. */
	epd_fb_draw_string(f, 48, 700, "Geometry", 3, BLACK, WHITE);
	epd_fb_fill_rect(f, 48, 750, 320, 220, BLACK);
	epd_fb_fill_rect(f, 68, 770, 280, 180, WHITE);
	epd_fb_fill_rect(f, 408, 750, 320, 220, 0x8);
	epd_fb_fill_rect(f, 768, 750, 320, 220, 0x4);

	/* Single-pixel rules, to show the panel resolves individual rows. */
	for (int i = 0; i < 10; i++) {
		epd_fb_fill_rect(f, 1140, 750 + i * 22, 320, 1, BLACK);
	}

	/* Frame the whole panel: any cropping or offset shows up immediately. */
	epd_fb_fill_rect(f, 0, 0, f->width, 3, BLACK);
	epd_fb_fill_rect(f, 0, f->height - 3, f->width, 3, BLACK);
	epd_fb_fill_rect(f, 0, 0, 3, f->height, BLACK);
	epd_fb_fill_rect(f, f->width - 3, 0, 3, f->height, BLACK);

	epd_fb_draw_string(f, 48, f->height - 130,
	                   "The border should be complete and the corners square.",
	                   2, BLACK, WHITE);
}

/*
 * DU darkening probe.
 *
 * A uniform shift in brightness is very hard to judge by eye - there is nothing
 * to compare against, and the panel you remember from a minute ago is not a
 * reliable reference. This gives the eye a reference by putting different
 * DOSES of DU next to each other on the same screen.
 *
 * Six horizontal bands, all starting from the same clean white, each given a
 * different number of no-op DU passes:
 *
 *     band 0    0 passes   <- untouched control
 *     band 1    1 pass
 *     band 2    2 passes
 *     band 3    5 passes
 *     band 4   10 passes
 *     band 5   20 passes
 *
 * Every pass is a NO-OP: the framebuffer is white everywhere, so every pixel is
 * above the DU threshold and receives the `hold` code. Nothing should be driven
 * and no band should change at all.
 *
 * How to read the result:
 *
 *   All bands identical           DU is not darkening anything. Whatever was
 *                                 seen earlier came from the content being
 *                                 drawn, not from the update itself.
 *   Progressive staircase         The darkening is dose-dependent: each pass
 *                                 adds a little. That points at the drive data
 *                                 or the per-pass gate/OE sequencing.
 *   All darken equally, including
 *   band 0                        Not the passes at all - something global,
 *                                 like the rails simply being up, or VCOM.
 *                                 Band 0 is the control that catches this.
 *
 * The band labels sit in a left margin that is OUTSIDE every update window, so
 * they are never inside a DU region and stay as a fixed reference.
 */
static void duProbe(epd_fb_t *f, const epd_panel_def_t *def)
{
	static const int kDose[]  = { 0, 1, 2, 5, 10, 20 };
	const int bands   = (int)(sizeof(kDose) / sizeof(kDose[0]));
	const int top     = 220;
	const int bandH   = 170;
	const int marginX = 300;          /* labels live left of this  */
	const int winX    = marginX;      /* windows start here        */
	const int winW    = f->width - marginX - 60;

	/* ---- Reference screen: clean white, labels, and a frame ---- */
	epd_fb_fill(f, WHITE);

	epd_fb_draw_string(f, 48, 60, "DU darkening probe", 4, BLACK, WHITE);
	epd_fb_draw_string(f, 48, 120,
	                   "All bands are no-op DU passes on white: nothing should change.",
	                   2, BLACK, WHITE);
	epd_fb_draw_string(f, 48, 155,
	                   "Compare bands against each other and against band 0.",
	                   2, BLACK, WHITE);

	for (int i = 0; i < bands; i++) {
		const int y = top + i * bandH;
		char lbl[32];

		snprintf(lbl, sizeof(lbl), "%2d pass%s", kDose[i],
		         kDose[i] == 1 ? "" : "es");
		epd_fb_draw_string(f, 48, y + bandH / 2 - 12, lbl, 3, BLACK, WHITE);

		/* Thin rules mark the band edges, so the boundary between two doses
		 * is unambiguous even when the difference is slight. */
		epd_fb_fill_rect(f, marginX, y, winW, 2, BLACK);
		epd_fb_fill_rect(f, marginX, y + bandH - 2, winW, 2, BLACK);

		/* A black square inside each band: if DU is darkening the white, the
		 * contrast against a true black changes too, which is easier to see
		 * than white alone. */
		epd_fb_fill_rect(f, marginX + 20, y + 30, 80, bandH - 60, BLACK);
	}

	epd_fb_fill_rect(f, 0, 0, f->width, 3, BLACK);
	epd_fb_fill_rect(f, 0, f->height - 3, f->width, 3, BLACK);
	epd_fb_fill_rect(f, 0, 0, 3, f->height, BLACK);
	epd_fb_fill_rect(f, f->width - 3, 0, 3, f->height, BLACK);

	Serial.println("Probe: drawing reference screen...");
	epd_panel_refresh(panel, NULL, f->buf, EPD_WAVEFORM_INIT);
	epd_panel_refresh(panel, NULL, f->buf, EPD_WAVEFORM_GC16);

	Serial.println("Probe: reference is on screen - look at it now.");
	delay(5000);

	/* ---- Apply the doses ---- */
	for (int i = 0; i < bands; i++) {
		const int y = top + i * bandH;

		if (kDose[i] == 0) {
			Serial.printf("  band %d: control, no passes\n", i);
			continue;
		}

		epd_rect_t area = {
			.x = (uint16_t)winX,
			.y = (uint16_t)(y + 4),
			.w = (uint16_t)winW,
			.h = (uint16_t)(bandH - 8),
		};

		Serial.printf("  band %d: %d no-op DU passes...\n", i, kDose[i]);
		for (int n = 0; n < kDose[i]; n++) {
			esp_err_t err = epd_panel_refresh_area(panel, NULL, f->buf,
			                                       EPD_WAVEFORM_DU, &area);
			if (err != ESP_OK) {
				Serial.printf("    DU failed: %s\n", esp_err_to_name(err));
				break;
			}
		}
	}

	Serial.println("Probe done.");
	Serial.println("  bands identical      -> DU is not the cause");
	Serial.println("  progressive staircase-> dose dependent, per-pass drive");
	Serial.println("  all equal inc band 0 -> global (rails/VCOM), not the passes");
}

/*
 * A full update: INIT then GC16.
 *
 * INIT before GC16 is not optional. GC16 phases can only darken a pixel, so the
 * greyscale model assumes it starts from a white panel. Without the INIT pass
 * the image lands on top of whatever the panel was holding and ghosts.
 */
static void showPage()
{
	uint32_t t0 = millis();
	epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_INIT);
	epd_panel_refresh(panel, NULL, fb.buf, EPD_WAVEFORM_GC16);
	Serial.printf("Full update in %lu ms\n", (unsigned long)(millis() - t0));
}

/*
 * A windowed DU update: no flash, the rest of the image untouched, and roughly
 * half the time of a full refresh.
 *
 * DU only darkens, so this can add ink but not remove it, which is why the bar
 * below grows rather than moving. Anything that has to disappear needs a full
 * refresh.
 */
static void growBar()
{
	const int barX = 48;
	const int barY = fb.height - 90;
	const int barH = 40;
	const int seg  = (fb.width - 96) / 8;

	for (int i = 0; i < 8; i++) {
		const int x = barX + i * seg;
		epd_fb_fill_rect(&fb, x, barY, seg - 4, barH, BLACK);

		/* Only the new segment is in the window: the rest of the bar is
		 * already on the glass and must not be redriven. */
		epd_rect_t area = {
			.x = (uint16_t)x,
			.y = (uint16_t)barY,
			.w = (uint16_t)seg,
			.h = (uint16_t)barH,
		};

		uint32_t t0 = millis();
		esp_err_t err = epd_panel_refresh_area(panel, NULL, fb.buf,
		                                       EPD_WAVEFORM_DU, &area);
		if (err != ESP_OK) {
			Serial.printf("DU update failed: %s\n", esp_err_to_name(err));
			return;
		}
		Serial.printf("  DU segment %d in %lu ms\n", i,
		              (unsigned long)(millis() - t0));
		delay(150);
	}
}

void setup()
{
	Serial.begin(115200);
	delay(200);
	Serial.println();
	Serial.println("epdInky ESP32-P4 - Arduino + epd driver");

	/*
	 * The PMIC is required - it supplies the panel rails.
	 *
	 * use_epd_gpio is off on purpose. That BSP helper parks the EPD pins as
	 * plain GPIO outputs, but the epd component configures the same pins
	 * itself, and it is the one that has to win.
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
	esp_err_t err = bsp_epdinky_init_with_config(&cfg, &board);
	if (err != ESP_OK) {
		Serial.printf("Board init failed: %s\n", esp_err_to_name(err));
		return;
	}

	/*
	 * Take the catalogue entry as-is.
	 *
	 * Copied rather than pointed at because the catalogue entries are static
	 * constants; the driver keeps its own copy, so a local is fine. VCOM lives
	 * in the definition on purpose - it belongs to the glass, and a wrong value
	 * degrades the image rather than erroring, so it should not be an
	 * application-level override that can silently drift.
	 */
	epd_panel_def_t panelDef = epd_panel_eink_ed103tc2;

	err = epd_display_panel_create(&panelDef, &kBoard, board.tps65185, &panel);
	if (err != ESP_OK) {
		Serial.printf("Panel create failed: %s\n", esp_err_to_name(err));
		return;
	}

	Serial.printf("Panel ready: %s %ux%u, %u-bit bus, VCOM -%u mV\n",
	              panelDef.name, panelDef.width, panelDef.height,
	              panelDef.bus_width, panelDef.vcom_mv);

	err = epd_fb_create(&fb, panelDef.width, panelDef.height);
	if (err != ESP_OK) {
		Serial.printf("Framebuffer alloc failed: %s\n", esp_err_to_name(err));
		return;
	}

	err = epd_panel_power_on(panel);
	if (err != ESP_OK) {
		Serial.printf("Power on failed: %s\n", esp_err_to_name(err));
		Serial.println("This is usually the panel not being connected, or "
		               "PWR_GOOD never arriving from the PMIC.");
		return;
	}

	Serial.printf("Deep clean: %d cycles...\n", CLEAN_CYCLES);
	uint32_t t0 = millis();
	epd_panel_clean(panel, CLEAN_CYCLES);
	Serial.printf("Deep clean done in %lu ms\n", (unsigned long)(millis() - t0));

#if DU_DARKENING_PROBE
	duProbe(&fb, &panelDef);
#else
	drawPage(&fb, &panelDef);
	showPage();

	Serial.println("Growing bar with windowed DU updates (no flash)...");
	growBar();
#endif

	/*
	 * Drop the rails. E-paper is bistable, so the image stays with the panel
	 * unpowered.
	 *
	 * Not only to save energy: leaving the rails up holds every pixel under a
	 * DC bias, and over minutes that makes white areas visibly grainy.
	 */
	epd_panel_power_off(panel);

	ready = true;
	lastUpdate = millis();
	Serial.printf("Redrawing every %d s\n", UPDATE_INTERVAL_S);
}

void loop()
{
#if DU_DARKENING_PROBE
	/* The probe result has to stay on the glass to be looked at, so nothing
	 * is redrawn over it. */
	delay(1000);
	return;
#else
	if (!ready) {
		delay(1000);
		return;
	}

	if (millis() - lastUpdate < (uint32_t)UPDATE_INTERVAL_S * 1000) {
		delay(100);
		return;
	}
	lastUpdate = millis();
	updateCount++;

	/* Rails have to come back up for the update, and go down again after. */
	esp_err_t err = epd_panel_power_on(panel);
	if (err != ESP_OK) {
		Serial.printf("Power on failed: %s\n", esp_err_to_name(err));
		return;
	}

	epd_panel_def_t panelDef = epd_panel_eink_ed103tc2;
	drawPage(&fb, &panelDef);
	showPage();
	growBar();

	epd_panel_power_off(panel);
#endif
}
