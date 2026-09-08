#include "bsp/EpdInky.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "EpdInky";

EpdInkyBoard EpdInky;

bool EpdInkyBoard::track(esp_err_t err)
{
	_lastError = err;
	return err == ESP_OK;
}

/* ===========================================================================
 * Lifecycle
 * ========================================================================= */

bool EpdInkyBoard::begin()
{
	EpdInkyOptions defaults;
	return begin(defaults);
}

bool EpdInkyBoard::begin(const EpdInkyOptions &options)
{
	if (_begun) {
		return true;
	}

	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tca6408  = options.gpioExpander;
	cfg.enable.use_tps65185 = options.pmic;
	cfg.enable.use_kxtj3    = options.accelerometer;
	cfg.enable.use_rv3028   = options.rtc;
	cfg.enable.use_stc3115  = options.fuelGauge;
	cfg.enable.use_epd_gpio = options.epdPins;
	cfg.enable.use_button   = options.button;
	cfg.enable.use_sdcard   = options.sdCard;
	cfg.opts.tps65185_power_up_rails = options.powerUpRails;

	if (!track(bsp_epdinky_init_with_config(&cfg, &_handles))) {
		ESP_LOGE(TAG, "begin() failed: %s", esp_err_to_name(_lastError));
		return false;
	}

	_begun = true;
	_sdMounted = options.sdCard;
	return true;
}

void EpdInkyBoard::end()
{
	if (!_begun) {
		return;
	}
	bsp_epdinky_deinit(&_handles);
	memset(&_handles, 0, sizeof(_handles));
	_sdMounted = false;
	_begun = false;
}

/* ===========================================================================
 * Sensors
 * ========================================================================= */

bool EpdInkyBoard::readAccel(EpdInkyAccel &out)
{
	if (!_handles.kxtj3) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}

	int16_t x = 0, y = 0, z = 0;
	if (!track(kxtj3_read_raw(_handles.kxtj3, &x, &y, &z))) {
		return false;
	}
	return track(kxtj3_raw_to_mg(_handles.kxtj3, x, y, z, &out.x, &out.y, &out.z));
}

bool EpdInkyBoard::readTime(EpdInkyTime &out)
{
	if (!_handles.rv3028) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}

	rv3028_time_t t{};
	if (!track(rv3028_get_time(_handles.rv3028, &t))) {
		return false;
	}
	out.year    = t.year;
	out.month   = t.month;
	out.day     = t.date;
	out.hour    = t.hours;
	out.minute  = t.minutes;
	out.second  = t.seconds;
	out.weekday = t.weekday;
	return true;
}

bool EpdInkyBoard::setTime(const EpdInkyTime &t)
{
	if (!_handles.rv3028) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}

	rv3028_time_t raw{};
	raw.year    = t.year;
	raw.month   = t.month;
	raw.date    = t.day;
	raw.hours   = t.hour;
	raw.minutes = t.minute;
	raw.seconds = t.second;
	raw.weekday = t.weekday;
	return track(rv3028_set_time(_handles.rv3028, &raw));
}

bool EpdInkyBoard::readBattery(EpdInkyBattery &out)
{
	if (!_handles.stc3115) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}

	stc3115_data_t d{};
	if (!track(stc3115_read_data(_handles.stc3115, &d))) {
		return false;
	}
	out.milliVolts = d.voltage_mv;
	out.microAmps  = d.current_ua;
	out.percent    = d.soc_permille / 10.0f;
	return true;
}

/* ===========================================================================
 * User I/O and expander
 * ========================================================================= */

bool EpdInkyBoard::buttonPressed()
{
	return bsp_button_is_pressed();
}

bool EpdInkyBoard::expanderWrite(int pin, bool level)
{
	if (!_handles.tca6408) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}
	return track(tca6408_set_output_pin(_handles.tca6408, pin, level ? 1 : 0));
}

int EpdInkyBoard::expanderRead(int pin)
{
	if (!_handles.tca6408 || pin < 0 || pin > 7) {
		_lastError = ESP_ERR_INVALID_ARG;
		return -1;
	}

	uint8_t value = 0;
	if (!track(tca6408_get_input_val(_handles.tca6408, &value))) {
		return -1;
	}
	return (value >> pin) & 1;
}

/* ===========================================================================
 * E-paper rails
 * ========================================================================= */

bool EpdInkyBoard::railsPowerUp()
{
	if (!_handles.tps65185) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}
	if (!track(tps65185_power_up(_handles.tps65185))) {
		return false;
	}
	return track(tps65185_vcom_enable(_handles.tps65185, true));
}

bool EpdInkyBoard::railsPowerDown()
{
	if (!_handles.tps65185) {
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}
	return track(tps65185_power_down(_handles.tps65185));
}

/* ===========================================================================
 * SD card
 * ========================================================================= */

bool EpdInkyBoard::sdBegin()
{
	if (_sdMounted) {
		return true;
	}
	if (!_handles.tca6408) {
		/* Card power is gated by the expander, so it must be up first. */
		ESP_LOGE(TAG, "sdBegin() needs the GPIO expander");
		_lastError = ESP_ERR_INVALID_STATE;
		return false;
	}
	if (!track(bsp_sdcard_mount(nullptr, _handles.tca6408))) {
		return false;
	}
	_sdMounted = true;
	return true;
}

void EpdInkyBoard::sdEnd()
{
	if (!_sdMounted) {
		return;
	}
	bsp_sdcard_unmount(_handles.tca6408);
	_sdMounted = false;
}

/* ===========================================================================
 * Wi-Fi
 * ========================================================================= */

bool EpdInkyBoard::wifiBegin()
{
	return track(bsp_wifi_init());
}

int EpdInkyBoard::wifiScan(bsp_wifi_ap_t *out, size_t max)
{
	size_t found = 0;
	if (!track(bsp_wifi_scan(out, max, &found))) {
		return -1;
	}
	return static_cast<int>(found);
}

void EpdInkyBoard::wifiScanPrint()
{
	track(bsp_wifi_scan_print());
}

bool EpdInkyBoard::wifiConnect(const char *ssid, const char *password, uint32_t timeoutMs)
{
	return track(bsp_wifi_connect(ssid, password, timeoutMs));
}

void EpdInkyBoard::wifiDisconnect()
{
	track(bsp_wifi_disconnect());
}

bool EpdInkyBoard::wifiConnected()
{
	return bsp_wifi_is_connected();
}

bool EpdInkyBoard::wifiLocalIP(char *out, size_t len)
{
	return bsp_wifi_get_ip_str(out, len) == ESP_OK;
}

/* ===========================================================================
 * Bluetooth LE
 * ========================================================================= */

bool EpdInkyBoard::bleBegin()
{
	return track(bsp_ble_init());
}

int EpdInkyBoard::bleScan(bsp_ble_device_t *out, size_t max, uint32_t durationMs)
{
	size_t found = 0;
	if (!track(bsp_ble_scan(out, max, &found, durationMs))) {
		return -1;
	}
	return static_cast<int>(found);
}

void EpdInkyBoard::bleScanPrint(uint32_t durationMs)
{
	track(bsp_ble_scan_print(durationMs));
}

/* ===========================================================================
 * Misc
 * ========================================================================= */

void EpdInkyBoard::i2cScan()
{
	track(bsp_i2c_scan());
}
