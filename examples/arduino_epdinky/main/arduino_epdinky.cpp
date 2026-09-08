/*
 * epdInky ESP32-P4/C6 - Arduino example.
 *
 * Uses the standard Arduino setup()/loop() entry points together with the
 * EpdInky wrapper around the board support package. Everything the plain-C BSP
 * exposes is available here, plus the usual Arduino APIs (Serial, millis, ...).
 *
 * Build with ESP-IDF (arduino-esp32 is pulled in as a managed component):
 *   idf.py set-target esp32p4
 *   idf.py build flash monitor
 */

#include <Arduino.h>

#include "bsp/EpdInky.h"

/* Optional: create wifi_credentials.h with DEMO_WIFI_SSID / DEMO_WIFI_PASSWORD
 * to let the button join a network. It is gitignored. */
#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#endif

static bool     lastButton = false;
static uint32_t lastReport = 0;

void setup()
{
	Serial.begin(115200);
	delay(200);
	Serial.println();
	Serial.println("epdInky ESP32-P4/C6 - Arduino example");

	EpdInkyOptions options;
	options.sdCard = true;         /* mount /sdcard during begin() */
	options.powerUpRails = false;  /* leave the e-paper rails off  */

	if (!EpdInky.begin(options)) {
		Serial.printf("Board init failed: %s\n", esp_err_to_name(EpdInky.lastError()));
		return;
	}

	Serial.println("Board ready. Devices present:");
	Serial.printf("  GPIO expander : %s\n", EpdInky.hasExpander()      ? "yes" : "no");
	Serial.printf("  e-Ink PMIC    : %s\n", EpdInky.hasPmic()          ? "yes" : "no");
	Serial.printf("  Accelerometer : %s\n", EpdInky.hasAccelerometer() ? "yes" : "no");
	Serial.printf("  RTC           : %s\n", EpdInky.hasRtc()           ? "yes" : "no");
	Serial.printf("  Fuel gauge    : %s\n", EpdInky.hasFuelGauge()     ? "yes" : "no");
	Serial.printf("  SD card       : %s\n", EpdInky.sdMounted()        ? "mounted" : "no");

	/* Write a file using the normal Arduino/POSIX file APIs. */
	if (EpdInky.sdMounted()) {
		FILE *f = fopen("/sdcard/arduino.txt", "w");
		if (f) {
			fprintf(f, "Written from an Arduino sketch on the ESP32-P4\n");
			fclose(f);
			Serial.println("Wrote /sdcard/arduino.txt");
		}
	}

	/* Wi-Fi and BLE both run on the ESP32-C6 over esp-hosted. */
	if (EpdInky.wifiBegin()) {
		Serial.println("Wi-Fi scan:");
		EpdInky.wifiScanPrint();
	}

	if (EpdInky.bleBegin()) {
		Serial.println("BLE scan:");
		EpdInky.bleScanPrint(5000);
	}

#ifdef DEMO_WIFI_SSID
	Serial.printf("Press the user button to join \"%s\"\n", DEMO_WIFI_SSID);
#else
	Serial.println("Press the user button to re-run a BLE scan.");
#endif
}

void loop()
{
	/* Report sensor readings once a second. */
	if (millis() - lastReport >= 1000) {
		lastReport = millis();

		EpdInkyAccel accel;
		if (EpdInky.readAccel(accel)) {
			Serial.printf("Accel: X=%.0f mg  Y=%.0f mg  Z=%.0f mg\n",
			              accel.x, accel.y, accel.z);
		}

		EpdInkyTime now;
		if (EpdInky.readTime(now)) {
			Serial.printf("RTC: %04u-%02u-%02u %02u:%02u:%02u\n",
			              now.year, now.month, now.day,
			              now.hour, now.minute, now.second);
		}

		EpdInkyBattery battery;
		if (EpdInky.readBattery(battery)) {
			Serial.printf("Battery: %u mV, %.1f%%\n",
			              battery.milliVolts, battery.percent);
		}

		if (EpdInky.wifiConnected()) {
			char ip[16];
			if (EpdInky.wifiLocalIP(ip, sizeof(ip))) {
				Serial.printf("Wi-Fi: connected, IP %s\n", ip);
			}
		}
	}

	/* Act on the press edge so holding the button does not retrigger. */
	bool pressed = EpdInky.buttonPressed();
	if (pressed && !lastButton) {
#ifdef DEMO_WIFI_SSID
		if (EpdInky.wifiConnected()) {
			Serial.println("Button: disconnecting Wi-Fi");
			EpdInky.wifiDisconnect();
		} else {
			Serial.printf("Button: joining \"%s\"...\n", DEMO_WIFI_SSID);
			if (EpdInky.wifiConnect(DEMO_WIFI_SSID, DEMO_WIFI_PASSWORD)) {
				char ip[16];
				EpdInky.wifiLocalIP(ip, sizeof(ip));
				Serial.printf("Joined, IP %s\n", ip);
			} else {
				Serial.printf("Join failed: %s\n", esp_err_to_name(EpdInky.lastError()));
				EpdInky.wifiDisconnect();
			}
		}
#else
		Serial.println("Button: BLE scan");
		EpdInky.bleScanPrint(3000);
#endif
	}
	lastButton = pressed;

	delay(20);
}
