/*
 * Arduino-style C++ wrapper for the epdInky ESP32-P4/C6 board.
 *
 * This sits on top of the plain-C BSP and is deliberately usable in three
 * places: an Arduino sketch, an Arduino-as-ESP-IDF-component project, and a
 * plain C++ ESP-IDF project. Only the small Arduino-specific conveniences at
 * the end require Arduino.h.
 *
 * A ready-made global instance called `EpdInky` is provided, matching the usual
 * Arduino library convention.
 */
/*
 * Note: this class deliberately exposes no Arduino String members. The BSP is
 * compiled once, without Arduino.h, so making members conditional on ARDUINO
 * would give the class a different definition in different translation units
 * and fail to link. Sketches can wrap the char* accessors in String themselves.
 */
#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <stddef.h>

#include "bsp/epdinky_p4_board.h"

/** @brief Which peripherals begin() should bring up. */
struct EpdInkyOptions {
	bool gpioExpander = true;   /**< required for SD card power and sensor IRQs */
	bool pmic         = true;   /**< TPS65185; rails stay off unless powerUpRails */
	bool accelerometer = true;  /**< optional part - skipped if not populated    */
	bool rtc          = true;   /**< optional part - skipped if not populated    */
	bool fuelGauge    = true;
	bool epdPins      = true;   /**< park the panel bus in a safe idle state     */
	bool button       = true;
	bool sdCard       = false;  /**< mount /sdcard (needs gpioExpander)          */
	bool powerUpRails = false;  /**< energise the e-paper rails during begin()   */
};

/** @brief Three-axis acceleration in milli-g. */
struct EpdInkyAccel {
	float x = 0;
	float y = 0;
	float z = 0;
};

/** @brief Wall-clock time from the RV-3028 RTC. */
struct EpdInkyTime {
	uint16_t year = 0;
	uint8_t  month = 0, day = 0;
	uint8_t  hour = 0, minute = 0, second = 0;
	uint8_t  weekday = 0;
};

/** @brief Battery state from the STC3115 fuel gauge. */
struct EpdInkyBattery {
	uint16_t milliVolts = 0;
	int32_t  microAmps  = 0;   /**< positive = charging */
	float    percent    = 0;   /**< state of charge, 0..100 */
};

class EpdInkyBoard {
public:
	/* ---- lifecycle ---- */

	/** @brief Bring up the board with the default set of peripherals. */
	bool begin();

	/** @brief Bring up exactly the peripherals selected in @p options. */
	bool begin(const EpdInkyOptions &options);

	/** @brief Release every peripheral brought up by begin(). */
	void end();

	/** @brief Error code from the last failed call, or ESP_OK. */
	esp_err_t lastError() const { return _lastError; }

	/* ---- presence ---- */

	bool hasAccelerometer() const { return _handles.kxtj3 != nullptr; }
	bool hasRtc()           const { return _handles.rv3028 != nullptr; }
	bool hasFuelGauge()     const { return _handles.stc3115 != nullptr; }
	bool hasExpander()      const { return _handles.tca6408 != nullptr; }
	bool hasPmic()          const { return _handles.tps65185 != nullptr; }

	/* ---- sensors ---- */

	bool readAccel(EpdInkyAccel &out);
	bool readTime(EpdInkyTime &out);
	bool setTime(const EpdInkyTime &t);
	bool readBattery(EpdInkyBattery &out);

	/* ---- user I/O ---- */

	bool buttonPressed();

	/* ---- GPIO expander (pins 0..7) ---- */

	bool expanderWrite(int pin, bool level);
	int  expanderRead(int pin);   /**< -1 on error */

	/* ---- e-paper power rails ---- */

	bool railsPowerUp();
	bool railsPowerDown();

	/* ---- SD card ---- */

	bool sdBegin();               /**< powers and mounts /sdcard */
	void sdEnd();
	bool sdMounted() const { return _sdMounted; }

	/* ---- Wi-Fi (ESP32-C6 over esp-hosted) ---- */

	bool wifiBegin();
	int  wifiScan(bsp_wifi_ap_t *out, size_t max);   /**< returns count, -1 on error */
	void wifiScanPrint();
	bool wifiConnect(const char *ssid, const char *password, uint32_t timeoutMs = 20000);
	void wifiDisconnect();
	bool wifiConnected();
	/** @brief Copy the IPv4 address into @p out (needs >= 16 bytes). */
	bool wifiLocalIP(char *out, size_t len);

	/* ---- Bluetooth LE ---- */

	bool bleBegin();
	int  bleScan(bsp_ble_device_t *out, size_t max, uint32_t durationMs = 5000);
	void bleScanPrint(uint32_t durationMs = 5000);

	/* ---- escape hatch ---- */

	/** @brief Raw BSP handles, for anything this wrapper does not expose. */
	bsp_epdinky_handles_t *handles() { return &_handles; }
	i2c_master_bus_handle_t i2cBus() { return _handles.i2c_bus; }

	/** @brief Print an i2cdetect-style map of the shared I2C bus. */
	void i2cScan();

private:
	bool                  _begun = false;
	bool                  _sdMounted = false;
	esp_err_t             _lastError = ESP_OK;
	bsp_epdinky_handles_t _handles{};

	bool track(esp_err_t err);
};

/** @brief Global instance, following the usual Arduino library convention. */
extern EpdInkyBoard EpdInky;

#endif /* __cplusplus */
