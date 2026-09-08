/*
 * SGM37604A backlight driver.
 *
 * The LED driver sits on the display module and is reached over the board's
 * shared I2C bus at 0x36, so the backlight cannot be touched until that bus is
 * up. There is no GPIO brightness control on this board - FPC1 pins 8 and 9 go
 * only to test points - so this is the only way to light the panel.
 */

#include "sgm37604a.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "sgm37604a";

/* LED_ENABLE turns the string on; MODE selects the ramp and PWM behaviour. */
#define SGM_LED_ENABLE_BITS 0x1F
#define SGM_MODE_DEFAULT    0x05

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
	uint8_t buf[2] = { reg, value };
	return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t sgm37604a_init(i2c_master_bus_handle_t bus, sgm37604a_current_t max_current)
{
	ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");

	/* Probe before adding the device, so a disconnected display module is
	 * reported once and clearly rather than as a run of failed writes. */
	ESP_RETURN_ON_ERROR(i2c_master_probe(bus, SGM37604A_I2C_ADDR, 100), TAG,
	                    "no SGM37604A at 0x%02X - is the display connected?",
	                    SGM37604A_I2C_ADDR);

	if (!s_dev) {
		i2c_device_config_t dev_cfg = {
			.dev_addr_length = I2C_ADDR_BIT_LEN_7,
			.device_address  = SGM37604A_I2C_ADDR,
			.scl_speed_hz    = SGM37604A_I2C_SPEED_HZ,
		};
		ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG,
		                    "could not add the backlight to the I2C bus");
	}

	ESP_RETURN_ON_ERROR(write_reg(SGM37604A_REG_LED_ENABLE, SGM_LED_ENABLE_BITS), TAG, "enable failed");
	ESP_RETURN_ON_ERROR(write_reg(SGM37604A_REG_MODE, SGM_MODE_DEFAULT), TAG, "mode failed");
	ESP_RETURN_ON_ERROR(write_reg(SGM37604A_REG_CURRENT, (uint8_t)max_current), TAG, "current failed");

	/* Start dark: the caller raises the brightness once there is something on
	 * screen, so the panel does not flash before the first frame. */
	ESP_RETURN_ON_ERROR(sgm37604a_set_brightness(0), TAG, "brightness failed");

	ESP_LOGI(TAG, "Backlight ready at 0x%02X", SGM37604A_I2C_ADDR);
	return ESP_OK;
}

esp_err_t sgm37604a_set_brightness(uint16_t level)
{
	ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");

	if (level > SGM37604A_MAX_BRIGHTNESS) {
		level = SGM37604A_MAX_BRIGHTNESS;
	}

	/*
	 * The 12-bit level splits as low nibble / high byte.
	 *
	 * Worth being explicit about: writing the level straight into the MSB
	 * register truncates it to 8 bits instead of shifting, and the panel then
	 * appears to wrap and dim again every time the level crosses a multiple
	 * of 256.
	 */
	uint8_t lsb = (uint8_t)(level & 0x0F);
	uint8_t msb = (uint8_t)((level >> 4) & 0xFF);

	ESP_RETURN_ON_ERROR(write_reg(SGM37604A_REG_BRIGHTNESS_LSB, lsb), TAG, "LSB write failed");
	ESP_RETURN_ON_ERROR(write_reg(SGM37604A_REG_BRIGHTNESS_MSB, msb), TAG, "MSB write failed");
	return ESP_OK;
}

esp_err_t sgm37604a_set_brightness_percent(uint8_t percent)
{
	if (percent > 100) {
		percent = 100;
	}
	return sgm37604a_set_brightness((uint16_t)((SGM37604A_MAX_BRIGHTNESS * percent) / 100));
}

esp_err_t sgm37604a_enable(bool on)
{
	ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
	return write_reg(SGM37604A_REG_LED_ENABLE, on ? SGM_LED_ENABLE_BITS : 0);
}

esp_err_t sgm37604a_get_faults(uint8_t *flags)
{
	ESP_RETURN_ON_FALSE(s_dev && flags, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
	uint8_t reg = SGM37604A_REG_FAULT_FLAGS;
	return i2c_master_transmit_receive(s_dev, &reg, 1, flags, 1, 100);
}

esp_err_t sgm37604a_deinit(void)
{
	if (!s_dev) {
		return ESP_OK;
	}
	sgm37604a_enable(false);
	esp_err_t err = i2c_master_bus_rm_device(s_dev);
	s_dev = NULL;
	return err;
}
