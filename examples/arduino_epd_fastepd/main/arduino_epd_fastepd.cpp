/*
 * epdInky ESP32-P4 - Arduino + FastEPD on a 10.3" e-paper panel
 *
 * Draws a status screen with the usual Arduino setup()/loop() entry points.
 * FastEPD owns the e-paper side of the board completely: the 16-bit parallel
 * bus, the row and gate timing, the greyscale waveforms and the TPS65185 PMIC.
 * The sketch only says which panel is fitted and draws.
 *
 * Build with ESP-IDF (arduino-esp32 and FastEPD are managed components):
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
 * Who owns I2C
 * ------------
 * FastEPD owns it here, and the BSP is deliberately not initialised.
 *
 * The IDF build of FastEPD adopts an existing bus - bbepI2CInit() calls
 * i2c_master_get_bus_handle(I2C_NUM_0) first - which is why the IDF example can
 * bring the BSP up and let FastEPD share it. The Arduino build takes a
 * different path in arduino_io.inl: it calls Wire.end() then Wire.begin(), so
 * it always creates its own bus. Doing that on a port the BSP already holds
 * fails with `i2c_new_master_bus failed: ESP_ERR_INVALID_STATE`, and every PMIC
 * access afterwards returns an I/O error.
 *
 * So this sketch lets FastEPD do the whole job. Nothing here needs the BSP's
 * other drivers; a sketch that did would have to bring them up on Wire rather
 * than through the C BSP.
 *
 *
 * Which panel definition
 * ----------------------
 * FastEPD ships a definition for this board: BB_PANEL_EPDINKY_P4_16. Its pin
 * map matches the schematic exactly, so no pins have to be described by hand.
 * The 16-bit variant is used because this board wires all of D0..D15.
 *
 * That definition deliberately leaves width and height at zero, because the
 * board is a carrier and the panel varies. bbepInitPanel() skips its buffer
 * allocation when the size is zero, so setPanelSize() must follow initPanel():
 * that call is what allocates the frame buffers and builds the greyscale
 * lookup tables.
 */

#include <Arduino.h>

#include "FastEPD.h"

/*
 * The panel fitted to this board.
 *
 * ED103TC2 is a 10.3" 1872x1404 panel. Size and VCOM both come from the glass,
 * so change them together if a different panel is fitted. VCOM is printed on
 * the panel's own flexible cable, and the wrong value gives a washed out image
 * or heavy ghosting rather than an error - so it fails quietly.
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

/*
 * Seconds between screen updates.
 *
 * A full update costs about a second and flashes the panel, so an e-paper UI
 * updates on a timer rather than continuously. 30 s is a reasonable pace for a
 * status screen.
 */
#define UPDATE_INTERVAL_S 30

static FASTEPD  epaper;
static uint32_t lastUpdate;
static uint32_t updateCount;

static const char *epdErr(int rc)
{
	switch (rc) {
	case BBEP_SUCCESS:              return "success";
	case BBEP_ERROR_BAD_PARAMETER:  return "bad parameter";
	case BBEP_ERROR_BAD_DATA:       return "bad data";
	case BBEP_ERROR_NOT_SUPPORTED:  return "not supported";
	case BBEP_ERROR_NO_MEMORY:      return "out of memory";
	case BBEP_ERROR_OUT_OF_BOUNDS:  return "out of bounds";
	case BBEP_IO_ERROR:             return "I/O error";
	default:                        return "unknown";
	}
}

/*
 * A dithered ramp, drawn as vertical bars of increasing density.
 *
 * In 1bpp the panel only resolves black and white, so apparent greys have to
 * come from how many pixels are set rather than from the drive level. Each
 * column below is drawn with a different line spacing, which reads as a ramp
 * from light to solid.
 */
static void drawRamp(int x, int y, int w, int h)
{
	const int steps = 8;
	const int step  = w / steps;

	for (int i = 0; i < steps; i++) {
		const int spacing = steps - i;   /* tighter lines look darker */
		for (int px = 0; px < step; px += spacing) {
			epaper.fillRect(x + i * step + px, y, 1, h, BBEP_BLACK);
		}
	}

	epaper.drawRect(x, y, step * steps, h, BBEP_BLACK);
}

static void drawScreen()
{
	epaper.fillScreen(BBEP_WHITE);
	epaper.setTextColor(BBEP_BLACK, BBEP_WHITE);

	/* Title */
	epaper.setFont(FONT_12x16);
	epaper.drawString("epdInky ESP32-P4  -  Arduino + FastEPD", 60, 60);

	epaper.setFont(FONT_8x8);
	char line[96];
	snprintf(line, sizeof(line), "%s   %dx%d   16-bit bus   VCOM %d mV",
	         EPD_PANEL_NAME, epaper.width(), epaper.height(), EPD_VCOM_MV);
	epaper.drawString(line, 60, 100);

	epaper.fillRect(60, 130, EPD_WIDTH - 120, 3, BBEP_BLACK);

	/* Live values, to show the screen is really being redrawn. */
	epaper.setFont(FONT_12x16);
	epaper.drawString("Status", 60, 180);

	epaper.setFont(FONT_8x8);
	snprintf(line, sizeof(line), "Update       : %lu", (unsigned long)updateCount);
	epaper.drawString(line, 60, 220);

	snprintf(line, sizeof(line), "Uptime       : %lu s",
	         (unsigned long)(millis() / 1000));
	epaper.drawString(line, 60, 245);

	snprintf(line, sizeof(line), "Free heap    : %lu bytes",
	         (unsigned long)ESP.getFreeHeap());
	epaper.drawString(line, 60, 270);

	snprintf(line, sizeof(line), "Free PSRAM   : %lu bytes",
	         (unsigned long)ESP.getFreePsram());
	epaper.drawString(line, 60, 295);

	snprintf(line, sizeof(line), "CPU          : %lu MHz",
	         (unsigned long)getCpuFrequencyMhz());
	epaper.drawString(line, 60, 320);

	/* Dithered ramp: 1bpp has no true greys, so density stands in for tone. */
	epaper.setFont(FONT_12x16);
	epaper.drawString("Dithered ramp", 60, 380);
	drawRamp(60, 420, EPD_WIDTH - 120, 160);

	/* Geometry, to confirm the coordinate system and that nothing is cropped
	 * or offset. */
	epaper.setFont(FONT_12x16);
	epaper.drawString("Geometry", 60, 630);

	epaper.fillRect(60, 680, 320, 220, BBEP_BLACK);
	epaper.fillRect(80, 700, 280, 180, BBEP_WHITE);
	epaper.drawRect(420, 680, 320, 220, BBEP_BLACK);
	epaper.drawLine(420, 680, 740, 900, BBEP_BLACK);
	epaper.drawLine(420, 900, 740, 680, BBEP_BLACK);

	/* Single-pixel rules, to show the panel resolves individual rows. */
	for (int i = 0; i < 10; i++) {
		epaper.drawLine(780, 680 + i * 22, 1100, 680 + i * 22, BBEP_BLACK);
	}

	/* White on black: the case that needs the panel driven both ways. */
	epaper.fillRect(1140, 680, EPD_WIDTH - 1200, 220, BBEP_BLACK);
	epaper.setTextColor(BBEP_WHITE, BBEP_BLACK);
	epaper.drawString("White on black", 1170, 760);
	epaper.setTextColor(BBEP_BLACK, BBEP_WHITE);

	/* Frame the whole panel: any cropping or offset shows up immediately. */
	epaper.drawRect(0, 0, EPD_WIDTH, EPD_HEIGHT, BBEP_BLACK);
	epaper.drawRect(1, 1, EPD_WIDTH - 2, EPD_HEIGHT - 2, BBEP_BLACK);

	epaper.setFont(FONT_8x8);
	epaper.drawString("The border should be complete and the corners square.",
	                  60, EPD_HEIGHT - 80);
}

void setup()
{
	Serial.begin(115200);
	delay(200);
	Serial.println();
	Serial.println("epdInky ESP32-P4 - Arduino + FastEPD");

	int rc = epaper.initPanel(BB_PANEL_EPDINKY_P4_16);
	if (rc != BBEP_SUCCESS) {
		Serial.printf("initPanel failed: %s\n", epdErr(rc));
		return;
	}

	/* The board definition carries no panel size, so it is supplied here
	 * together with the VCOM the fitted glass needs. This call is also what
	 * allocates the frame buffers. */
	rc = epaper.setPanelSize(EPD_WIDTH, EPD_HEIGHT, EPD_FLAGS, EPD_VCOM_MV);
	if (rc != BBEP_SUCCESS) {
		Serial.printf("setPanelSize failed: %s\n", epdErr(rc));
		return;
	}

	Serial.printf("Panel ready: %s %dx%d, VCOM %d mV\n",
	              EPD_PANEL_NAME, epaper.width(), epaper.height(), EPD_VCOM_MV);

	/*
	 * 1bpp.
	 *
	 * The 4bpp greyscale mode needs a waveform this panel does not ship, and
	 * renders blank. 1bpp is the mode that works here; greys come out as
	 * dithered patterns rather than true tones.
	 */
	epaper.setMode(BB_MODE_1BPP);

	/*
	 * Clear to white first.
	 *
	 * E-paper holds whatever it was last showing, including from before a
	 * power cycle, and drawing on top of an unknown image leaves ghosting.
	 */
	Serial.println("Clearing to white...");
	rc = epaper.clearWhite(true);   /* keep the rails up for the update below */
	if (rc != BBEP_SUCCESS) {
		Serial.printf("clearWhite failed: %s\n", epdErr(rc));
		Serial.println("An I/O error here is usually the panel not being "
		               "connected, or PWR_GOOD never arriving from the PMIC.");
		return;
	}

	Serial.println("Drawing...");
	drawScreen();

	uint32_t t0 = millis();
	rc = epaper.fullUpdate(CLEAR_SLOW, false);
	if (rc != BBEP_SUCCESS) {
		Serial.printf("fullUpdate failed: %s\n", epdErr(rc));
		return;
	}
	Serial.printf("Screen shown in %lu ms\n", (unsigned long)(millis() - t0));

	/*
	 * The rails are down and the image stays: e-paper is bistable, so the
	 * panel needs no power to hold what it is showing.
	 */
	lastUpdate = millis();
	Serial.printf("Redrawing every %d s\n", UPDATE_INTERVAL_S);
}

void loop()
{
	if (millis() - lastUpdate < (uint32_t)UPDATE_INTERVAL_S * 1000) {
		delay(100);
		return;
	}
	lastUpdate = millis();
	updateCount++;

	drawScreen();

	uint32_t t0 = millis();
	int rc = epaper.fullUpdate(CLEAR_SLOW, false);
	if (rc != BBEP_SUCCESS) {
		Serial.printf("fullUpdate failed: %s\n", epdErr(rc));
		return;
	}
	Serial.printf("Update %lu shown in %lu ms\n",
	              (unsigned long)updateCount, (unsigned long)(millis() - t0));
}
