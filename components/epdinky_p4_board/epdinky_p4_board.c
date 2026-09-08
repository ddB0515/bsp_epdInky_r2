#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "bsp/epdinky_p4_board.h"

static const char *TAG = "epdinky_p4";

/* Shared I2C bus, brought up on demand by any component init. */
static i2c_master_bus_handle_t s_i2c_bus;
static sdmmc_card_t           *s_sd_card;
static gpio_num_t              s_button_io = GPIO_NUM_NC;

/* ===========================================================================
 * Small GPIO helpers
 * ========================================================================= */

static esp_err_t gpio_setup(gpio_num_t pin, gpio_mode_t mode, bool pull_up)
{
	if (pin == GPIO_NUM_NC) {
		return ESP_OK;
	}
	gpio_config_t io_conf = {
		.pin_bit_mask = 1ULL << pin,
		.mode         = mode,
		.pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type    = GPIO_INTR_DISABLE,
	};
	return gpio_config(&io_conf);
}

static esp_err_t gpio_setup_output(gpio_num_t pin, int level)
{
	if (pin == GPIO_NUM_NC) {
		return ESP_OK;
	}
	esp_err_t err = gpio_setup(pin, GPIO_MODE_OUTPUT, false);
	if (err != ESP_OK) {
		return err;
	}
	return gpio_set_level(pin, level);
}

/* ===========================================================================
 * Core: I2C bus
 * ========================================================================= */

bsp_epdinky_config_t bsp_epdinky_default_config(void)
{
	static const gpio_num_t epd_data_default[BSP_EPD_DATA_PIN_COUNT] = BSP_EPD_DATA_PINS_DEFAULT;

	bsp_epdinky_config_t cfg = {
		.i2c = {
			.port           = BSP_I2C_NUM,
			.sda_io         = BSP_I2C_SDA_IO,
			.scl_io         = BSP_I2C_SCL_IO,
			.freq_hz        = BSP_I2C_FREQ_HZ,
			.enable_pullups = BSP_I2C_INTERNAL_PULLUPS,
		},
		.gpio = {
			.tps65185 = {
				.pwrgood_io   = BSP_TPS65185_PIN_PWRGOOD,
				.power_up_io  = BSP_TPS65185_PIN_POWER_UP,
				.int_io       = BSP_TPS65185_PIN_INT,
				.vcom_ctrl_io = BSP_TPS65185_PIN_VCOM_CTRL,
				.wake_up_io   = BSP_TPS65185_PIN_WAKE_UP,
			},
			.tca6408 = {
				.int_io = BSP_TCA6408_PIN_INT_IO,
			},
			.epd = {
				.data_io = {0},
				.spv_io  = BSP_EPD_PIN_SPV,
				.ckv_io  = BSP_EPD_PIN_CKV,
				.mode_io = BSP_EPD_PIN_MODE,
				.xstl_io = BSP_EPD_PIN_XSTL,
				.xoe_io  = BSP_EPD_PIN_XOE,
				.xle_io  = BSP_EPD_PIN_XLE,
				.xcl_io  = BSP_EPD_PIN_XCL,
			},
			.sd = {
				.cmd_io             = BSP_SD_PIN_CMD,
				.clk_io             = BSP_SD_PIN_CLK,
				.d0_io              = BSP_SD_PIN_D0,
				.d1_io              = BSP_SD_PIN_D1,
				.d2_io              = BSP_SD_PIN_D2,
				.d3_io              = BSP_SD_PIN_D3,
				.slot               = BSP_SD_SLOT,
				.power_expander_pin = BSP_TCA6408_PIN_SD_EN,
			},
			.button_io = BSP_BUTTON_PIN,
		},
		.addrs = {
			.kxtj3     = I2C_DEVICE_KXTJ3_ADDR,
			.kxtj3_alt = I2C_DEVICE_KXTJ3_ADDR_ALT,
			.rv3028    = I2C_DEVICE_RV3028_ADDR,
			.tps65185  = I2C_DEVICE_TPS65185_ADDR,
			.tca6408   = I2C_DEVICE_TCA6408_ADDR,
			.stc3115   = I2C_DEVICE_STC3115_ADDR,
		},
		.opts = {
			.kxtj3_range_g          = 2,
			.kxtj3_odr_hz           = 50,
			.rv3028_trickle_chg_en  = false,
			.stc3115_capacity_mAh   = 500,
			.stc3115_sense_mohm     = 10,
			.tps65185_power_up_rails = false,
			.tps65185_vcom_mV       = 1500,
			.tps65185_vpos_vneg_mV  = 15000,
		},
		.enable = {
			.use_tca6408  = true,
			.use_tps65185 = true,
			.use_stc3115  = true,
			.use_kxtj3    = true,
			.use_rv3028   = true,
			.use_epd_gpio = true,
			.use_button   = true,
			.use_sdcard   = false,
		},
		.allow_missing_required_devices = false,
	};

	memcpy(cfg.gpio.epd.data_io, epd_data_default, sizeof(cfg.gpio.epd.data_io));
	return cfg;
}

esp_err_t bsp_i2c_init(const bsp_epdinky_i2c_cfg_t *cfg)
{
	if (s_i2c_bus) {
		return ESP_OK;
	}

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults.i2c;
	}

	i2c_master_bus_config_t bus_config = {
		.i2c_port                     = cfg->port,
		.sda_io_num                   = cfg->sda_io,
		.scl_io_num                   = cfg->scl_io,
		.clk_source                   = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt            = 7,
		.flags.enable_internal_pullup = cfg->enable_pullups,
	};

	esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
	ESP_RETURN_ON_ERROR(err, TAG, "i2c bus init failed");

	ESP_LOGI(TAG, "I2C bus up on port %d (SDA=%d SCL=%d, %" PRIu32 " Hz)",
	         (int)cfg->port, cfg->sda_io, cfg->scl_io, cfg->freq_hz);
	return ESP_OK;
}

esp_err_t bsp_i2c_get_handle(i2c_master_bus_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
	ESP_RETURN_ON_ERROR(bsp_i2c_init(NULL), TAG, "i2c init failed");
	*out = s_i2c_bus;
	return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
	if (!s_i2c_bus) {
		return ESP_OK;
	}
	esp_err_t err = i2c_del_master_bus(s_i2c_bus);
	if (err == ESP_OK) {
		s_i2c_bus = NULL;
	}
	return err;
}

esp_err_t bsp_i2c_probe(uint8_t addr)
{
	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "i2c not available");
	return i2c_master_probe(bus, addr, 50);
}

esp_err_t bsp_i2c_scan(void)
{
	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "i2c not available");

	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for (int base = 0; base < 128; base += 16) {
		printf("%02x: ", base);
		for (int off = 0; off < 16; off++) {
			uint8_t addr = base + off;
			esp_err_t ret = i2c_master_probe(bus, addr, 20);
			if (ret == ESP_OK) {
				printf("%02x ", addr);
			} else if (ret == ESP_ERR_TIMEOUT) {
				printf("UU ");
			} else {
				printf("-- ");
			}
		}
		printf("\n");
	}
	return ESP_OK;
}

/* Shared guard: make sure the bus is up and the device actually answers. */
static esp_err_t require_device(uint8_t addr, const char *name, i2c_master_bus_handle_t *bus)
{
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(bus), TAG, "i2c not available");

	if (i2c_master_probe(*bus, addr, 50) != ESP_OK) {
		ESP_LOGW(TAG, "%s not responding at 0x%02x", name, addr);
		return ESP_ERR_NOT_FOUND;
	}
	return ESP_OK;
}

/* ===========================================================================
 * TCA6408A GPIO expander
 * ========================================================================= */

esp_err_t bsp_tca6408_init(const bsp_epdinky_config_t *cfg, tca6408_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(require_device(cfg->addrs.tca6408, "TCA6408", &bus), TAG, "probe failed");

	/* The aggregate interrupt line is open-drain with an external pull-up. */
	ESP_RETURN_ON_ERROR(gpio_setup(cfg->gpio.tca6408.int_io, GPIO_MODE_INPUT, false),
	                    TAG, "expander INT gpio failed");

	tca6408_handle_t handle = NULL;
	ESP_RETURN_ON_ERROR(tca6408_init(bus, cfg->addrs.tca6408, &handle), TAG, "tca6408 init failed");

	/* Drive the output latch before switching P7 to an output so the SD power
	 * gate never glitches on. */
	esp_err_t err = tca6408_set_output_val(handle, BSP_TCA6408_OUT_DEFAULT);
	if (err == ESP_OK) {
		err = tca6408_set_polarity(handle, 0x00);
	}
	if (err == ESP_OK) {
		err = tca6408_set_config(handle, BSP_TCA6408_DIR_DEFAULT);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "TCA6408 configuration failed: %s", esp_err_to_name(err));
		tca6408_delete(handle);
		return err;
	}

	ESP_LOGI(TAG, "TCA6408 ready at 0x%02x (SD power off, INT on GPIO%d)",
	         cfg->addrs.tca6408, (int)cfg->gpio.tca6408.int_io);
	*out = handle;
	return ESP_OK;
}

/* ===========================================================================
 * TPS65185 e-Ink PMIC
 * ========================================================================= */

esp_err_t bsp_tps65185_init(const bsp_epdinky_config_t *cfg, tps65185_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	const bsp_epdinky_tps65185_gpio_t *pins = &cfg->gpio.tps65185;

	/* WAKEUP must be asserted before the PMIC answers on I2C. */
	ESP_RETURN_ON_ERROR(gpio_setup_output(pins->wake_up_io, 1), TAG, "WAKEUP gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(pins->power_up_io, 0), TAG, "PWRUP gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(pins->vcom_ctrl_io, 0), TAG, "VCOM_CTRL gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup(pins->int_io, GPIO_MODE_INPUT, false), TAG, "INT gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup(pins->pwrgood_io, GPIO_MODE_INPUT, false), TAG, "PWRGOOD gpio failed");
	vTaskDelay(pdMS_TO_TICKS(10));

	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(require_device(cfg->addrs.tps65185, "TPS65185", &bus), TAG, "probe failed");

	tps65185_gpio_config_t drv_gpio = {
		.wakeup_pin    = pins->wake_up_io,
		.pwrup_pin     = pins->power_up_io,
		.vcom_ctrl_pin = pins->vcom_ctrl_io,
		.int_pin       = pins->int_io,
		.pwr_good_pin  = pins->pwrgood_io,
	};

	tps65185_handle_t handle = NULL;
	ESP_RETURN_ON_ERROR(tps65185_init(bus, &drv_gpio, &handle), TAG, "tps65185 init failed");

	uint8_t rev = 0;
	if (tps65185_get_revid(handle, &rev) == ESP_OK) {
		ESP_LOGI(TAG, "TPS65185 ready at 0x%02x (rev 0x%02x)", cfg->addrs.tps65185, rev);
	}

	if (cfg->opts.tps65185_vcom_mV > 0) {
		esp_err_t err = tps65185_set_vcom(handle, (uint16_t)cfg->opts.tps65185_vcom_mV);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "TPS65185 set VCOM failed: %s", esp_err_to_name(err));
			tps65185_deinit(handle);
			return err;
		}
	}

	if (cfg->opts.tps65185_power_up_rails) {
		/* Map the requested rail voltage; the part only supports 12-15 V. */
		tps65185_vset_t vset;
		switch (cfg->opts.tps65185_vpos_vneg_mV) {
		case 15000: vset = TPS65185_VSET_15V; break;
		case 14000: vset = TPS65185_VSET_14V; break;
		case 13000: vset = TPS65185_VSET_13V; break;
		case 12000: vset = TPS65185_VSET_12V; break;
		default:
			ESP_LOGE(TAG, "Unsupported VPOS/VNEG %d mV (use 12000/13000/14000/15000)",
			         cfg->opts.tps65185_vpos_vneg_mV);
			tps65185_deinit(handle);
			return ESP_ERR_INVALID_ARG;
		}

		esp_err_t err = tps65185_set_vpos_vneg(handle, vset);
		if (err == ESP_OK) {
			err = tps65185_power_up(handle);
		}
		if (err == ESP_OK) {
			err = tps65185_vcom_enable(handle, true);
		}
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "TPS65185 rail power-up failed: %s", esp_err_to_name(err));
			tps65185_power_down(handle);
			tps65185_deinit(handle);
			return err;
		}
		ESP_LOGI(TAG, "TPS65185 rails powered up at +/-%d mV",
		         cfg->opts.tps65185_vpos_vneg_mV);
	} else {
		ESP_LOGI(TAG, "TPS65185 rails left off (set opts.tps65185_power_up_rails to enable)");
	}

	*out = handle;
	return ESP_OK;
}

/* ===========================================================================
 * KXTJ3-1057 accelerometer
 * ========================================================================= */

esp_err_t bsp_kxtj3_init(const bsp_epdinky_config_t *cfg, kxtj3_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "i2c not available");

	/* ADDR is strapped by jumper JP2, so accept either address. */
	uint8_t addr = cfg->addrs.kxtj3;
	if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
		addr = cfg->addrs.kxtj3_alt;
		if (addr == 0 || i2c_master_probe(bus, addr, 50) != ESP_OK) {
			ESP_LOGW(TAG, "KXTJ3 not responding at 0x%02x or 0x%02x",
			         cfg->addrs.kxtj3, cfg->addrs.kxtj3_alt);
			return ESP_ERR_NOT_FOUND;
		}
	}

	kxtj3_handle_t handle = NULL;
	ESP_RETURN_ON_ERROR(kxtj3_init(bus, addr, &handle), TAG, "kxtj3 init failed");

	kxtj3_range_t range = KXTJ3_RANGE_2G;
	if (cfg->opts.kxtj3_range_g == 4) {
		range = KXTJ3_RANGE_4G;
	} else if (cfg->opts.kxtj3_range_g == 8) {
		range = KXTJ3_RANGE_8G;
	}

	/* INT is wired to expander P0, not to the P4, so leave the data-ready
	 * interrupt off unless the caller wires it up through the expander. */
	esp_err_t err = kxtj3_configure(handle, range, cfg->opts.kxtj3_odr_hz, true, false);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "KXTJ3 configure failed: %s", esp_err_to_name(err));
		kxtj3_delete(handle, bus);
		return err;
	}

	ESP_LOGI(TAG, "KXTJ3 ready at 0x%02x (+/-%dg, %d Hz)",
	         addr, cfg->opts.kxtj3_range_g, cfg->opts.kxtj3_odr_hz);
	*out = handle;
	return ESP_OK;
}

/* ===========================================================================
 * RV-3028-C7 RTC
 * ========================================================================= */

esp_err_t bsp_rv3028_init(const bsp_epdinky_config_t *cfg, rv3028_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(require_device(cfg->addrs.rv3028, "RV3028", &bus), TAG, "probe failed");

	rv3028_handle_t handle = NULL;
	ESP_RETURN_ON_ERROR(rv3028_init(bus, cfg->addrs.rv3028, &handle), TAG, "rv3028 init failed");

	if (cfg->opts.rv3028_trickle_chg_en) {
		esp_err_t err = rv3028_set_trickle_charge(handle, true, RV3028_TCR_3K);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "RV3028 trickle charge setup failed: %s", esp_err_to_name(err));
		}
	}

	ESP_LOGI(TAG, "RV3028 ready at 0x%02x", cfg->addrs.rv3028);
	*out = handle;
	return ESP_OK;
}

/* ===========================================================================
 * STC3115 fuel gauge
 * ========================================================================= */

esp_err_t bsp_stc3115_init(const bsp_epdinky_config_t *cfg, stc3115_handle_t *out)
{
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	i2c_master_bus_handle_t bus;
	ESP_RETURN_ON_ERROR(require_device(cfg->addrs.stc3115, "STC3115", &bus), TAG, "probe failed");

	stc3115_config_t gauge_cfg = {
		.battery_capacity_mah = (cfg->opts.stc3115_capacity_mAh > 0)
		                        ? cfg->opts.stc3115_capacity_mAh : 500,
		.sense_resistor_mohm  = (cfg->opts.stc3115_sense_mohm > 0)
		                        ? cfg->opts.stc3115_sense_mohm : 10,
		.alarm_soc            = 0,
		.alarm_voltage_mv     = 0,
		.current_thres        = STC3115_DEFAULT_CURRENT_THRES,
		.relax_max            = STC3115_DEFAULT_RELAX_MAX,
		.voltage_mode         = false,
		.ocv_table            = NULL,
	};

	stc3115_handle_t handle = NULL;
	ESP_RETURN_ON_ERROR(stc3115_init(bus, &gauge_cfg, &handle, NULL), TAG, "stc3115 init failed");

	ESP_LOGI(TAG, "STC3115 ready at 0x%02x (%d mAh, %d mOhm sense)",
	         cfg->addrs.stc3115, gauge_cfg.battery_capacity_mah, gauge_cfg.sense_resistor_mohm);
	*out = handle;
	return ESP_OK;
}

/* ===========================================================================
 * EPD parallel bus / user button
 * ========================================================================= */

esp_err_t bsp_epd_gpio_init(const bsp_epdinky_config_t *cfg)
{
	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	const bsp_epdinky_epd_gpio_t *epd = &cfg->gpio.epd;

	for (int i = 0; i < BSP_EPD_DATA_PIN_COUNT; i++) {
		ESP_RETURN_ON_ERROR(gpio_setup_output(epd->data_io[i], 0), TAG, "EPD data gpio failed");
	}

	/* XOE low keeps the source drivers disabled; MODE low keeps the gate
	 * drivers idle. Everything else parks low. */
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->spv_io, 0), TAG, "SPV gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->ckv_io, 0), TAG, "CKV gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->mode_io, 0), TAG, "MODE gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->xstl_io, 0), TAG, "XSTL gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->xoe_io, 0), TAG, "XOE gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->xle_io, 0), TAG, "XLE gpio failed");
	ESP_RETURN_ON_ERROR(gpio_setup_output(epd->xcl_io, 0), TAG, "XCL gpio failed");

	ESP_LOGI(TAG, "EPD bus parked (D0..D15 on GPIO%d..GPIO%d)",
	         (int)epd->data_io[0], (int)epd->data_io[BSP_EPD_DATA_PIN_COUNT - 1]);
	return ESP_OK;
}

esp_err_t bsp_button_init(const bsp_epdinky_config_t *cfg)
{
	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	/* External 10K pull-up (R45) is present; the internal one is harmless
	 * but unnecessary. */
	ESP_RETURN_ON_ERROR(gpio_setup(cfg->gpio.button_io, GPIO_MODE_INPUT, false),
	                    TAG, "button gpio failed");
	s_button_io = cfg->gpio.button_io;
	ESP_LOGI(TAG, "User button ready on GPIO%d", (int)s_button_io);
	return ESP_OK;
}

bool bsp_button_is_pressed(void)
{
	if (s_button_io == GPIO_NUM_NC) {
		return false;
	}
	return gpio_get_level(s_button_io) == BSP_BUTTON_ACTIVE_LEVEL;
}

/* ===========================================================================
 * micro-SD card
 * ========================================================================= */

esp_err_t bsp_sdcard_set_power(tca6408_handle_t tca, bool on)
{
	ESP_RETURN_ON_FALSE(tca, ESP_ERR_INVALID_ARG, TAG,
	                    "SD power is gated by the TCA6408 - init the expander first");

	/* Q4 is a P-channel FET: pulling the gate low turns the card on. */
	int level = on ? BSP_SD_EN_ACTIVE_LEVEL : !BSP_SD_EN_ACTIVE_LEVEL;
	ESP_RETURN_ON_ERROR(tca6408_set_output_pin(tca, BSP_TCA6408_PIN_SD_EN, level),
	                    TAG, "SD power switch failed");

	ESP_LOGI(TAG, "SD card power %s", on ? "on" : "off");
	vTaskDelay(pdMS_TO_TICKS(on ? 20 : 5));
	return ESP_OK;
}

/*
 * The ESP32-P4 has a single SDMMC controller shared by both slots, and
 * sdmmc_host_init() is not reference counted - it unconditionally creates a
 * controller and fails with ESP_ERR_NOT_FOUND if one already exists.
 *
 * When Wi-Fi is enabled, esp-hosted claims that controller for the ESP32-C6 on
 * slot 1 before app_main() runs. Treat "already created" as success so our card
 * on slot 0 attaches to the existing controller instead of failing outright.
 *
 * Teardown needs no equivalent shim: SDMMC_HOST_DEFAULT() sets deinit_p to
 * sdmmc_host_deinit_slot(), which drops only our slot.
 */
static esp_err_t bsp_sdmmc_host_init_shared(void)
{
	/* sdmmc_host_init() logs at ERROR level when the controller already
	 * exists, which is the normal case here and not a failure. Mute the two
	 * tags it logs under for the duration of the call so the boot log does
	 * not show alarming errors for an expected condition. */
	esp_log_level_t prev_sd_host = esp_log_level_get("SD_HOST");
	esp_log_level_t prev_periph  = esp_log_level_get("sdmmc_periph");
	esp_log_level_set("SD_HOST", ESP_LOG_NONE);
	esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);

	esp_err_t err = sdmmc_host_init();

	esp_log_level_set("SD_HOST", prev_sd_host);
	esp_log_level_set("sdmmc_periph", prev_periph);

	if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
		ESP_LOGI(TAG, "Sharing the SDMMC controller already created by esp-hosted");
		return ESP_OK;
	}
	if (err != ESP_OK) {
		/* Re-report anything unexpected, since the real message was muted. */
		ESP_LOGE(TAG, "sdmmc_host_init failed: %s", esp_err_to_name(err));
	}
	return err;
}

/*
 * UPSTREAM BUG WORKAROUND (ESP-IDF 6.0.2)
 *
 * sd_host_isr() picks up the current slot without checking it exists:
 *
 *     sd_host_sdmmc_slot_t *slot = ctlr->slot[ctlr->cur_slot_id];
 *     ...
 *     if (slot->cbs.on_trans_done)          <-- offset 0x9c
 *
 * cur_slot_id is set per transaction (sd_trans_sdmmc.c) and is never reset when
 * a slot is removed, while removing a slot sets ctlr->slot[id] = NULL
 * (sd_host_sdmmc.c). Releasing our slot while the controller stays alive
 * therefore leaves that ISR dereferencing NULL.
 *
 * That is exactly what happens when no card is fitted: the mount fails after
 * doing transactions on slot 0 (so cur_slot_id == 0), IDF's cleanup releases
 * slot 0, and the very next SDIO interrupt - from esp-hosted's Wi-Fi traffic on
 * slot 1, which keeps the controller busy - panics with
 * "Load access fault ... MTVAL 0x9c". Reproduced on hardware.
 *
 * So the slot is retained rather than released. Nothing else on this board
 * wants those pins, and card detect is not routed, so the card is only ever
 * mounted at boot. If mount is called again, the retained slot is handed back
 * first, which is safe by then because esp-hosted's ongoing traffic has long
 * since moved cur_slot_id to its own slot.
 */
static bool s_sdmmc_slot_retained;
static int  s_sdmmc_retained_slot;

static esp_err_t bsp_sdmmc_slot_retain(int slot)
{
	s_sdmmc_slot_retained = true;
	s_sdmmc_retained_slot = slot;
	ESP_LOGD(TAG, "retaining SDMMC slot %d (sd_host_isr would fault otherwise)", slot);
	return ESP_OK;
}

static void bsp_sdmmc_slot_release_retained(void)
{
	if (!s_sdmmc_slot_retained) {
		return;
	}

	/* Releasing the last slot we own makes the driver try to delete the whole
	 * controller, which fails because esp-hosted still holds its own slot.
	 * That is expected here, so mute the error it logs. */
	esp_log_level_t prev = esp_log_level_get("SD_HOST");
	esp_log_level_set("SD_HOST", ESP_LOG_NONE);
	sdmmc_host_deinit_slot(s_sdmmc_retained_slot);
	esp_log_level_set("SD_HOST", prev);

	s_sdmmc_slot_retained = false;
}

esp_err_t bsp_sdcard_mount(const bsp_epdinky_config_t *cfg, tca6408_handle_t tca)
{
	if (s_sd_card) {
		return ESP_OK;
	}

	bsp_epdinky_config_t defaults;
	if (cfg == NULL) {
		defaults = bsp_epdinky_default_config();
		cfg = &defaults;
	}

	/* Hand back a slot kept from an earlier attempt so it can be registered
	 * again below. */
	bsp_sdmmc_slot_release_retained();

	ESP_RETURN_ON_ERROR(bsp_sdcard_set_power(tca, true), TAG, "SD power up failed");

	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.slot = cfg->gpio.sd.slot;
	host.init = bsp_sdmmc_host_init_shared;
	host.deinit_p = bsp_sdmmc_slot_retain;

	sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
	slot_config.width   = 4;
	slot_config.clk     = cfg->gpio.sd.clk_io;
	slot_config.cmd     = cfg->gpio.sd.cmd_io;
	slot_config.d0      = cfg->gpio.sd.d0_io;
	slot_config.d1      = cfg->gpio.sd.d1_io;
	slot_config.d2      = cfg->gpio.sd.d2_io;
	slot_config.d3      = cfg->gpio.sd.d3_io;
	/* SD1-CD is not routed to the MCU, so card detect is unavailable. */
	slot_config.cd      = SDMMC_SLOT_NO_CD;
	slot_config.wp      = SDMMC_SLOT_NO_WP;
	/* R22-R27 provide external 10K pull-ups, so no internal ones are needed. */

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files              = 5,
		.allocation_unit_size   = 16 * 1024,
	};

	esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config,
	                                        &mount_config, &s_sd_card);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
		s_sd_card = NULL;
		bsp_sdcard_set_power(tca, false);
		return err;
	}

	ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
	sdmmc_card_print_info(stdout, s_sd_card);
	return ESP_OK;
}

esp_err_t bsp_sdcard_unmount(tca6408_handle_t tca)
{
	esp_err_t err = ESP_OK;

	if (s_sd_card) {
		err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_sd_card);
		s_sd_card = NULL;
	}
	/* Drop card power even if it was only ever enabled via bsp_sdcard_set_power(). */
	if (tca) {
		esp_err_t power_err = bsp_sdcard_set_power(tca, false);
		if (err == ESP_OK) {
			err = power_err;
		}
	}
	return err;
}

/* ===========================================================================
 * Whole-board init
 * ========================================================================= */

/*
 * Fold a per-component result into the overall init result.
 *
 * Whether a missing device is fatal is a property of the board, not of the
 * caller: the schematic marks the accelerometer and RTC as OPTIONAL (they may
 * be depopulated), while the expander and PMIC are required for the board to
 * function at all. Passing that in per device means a missing accelerometer
 * never aborts init, and a missing PMIC is never silently ignored.
 *
 * cfg->allow_missing_required_devices overrides this for bench bring-up on a
 * partially populated board.
 */
static esp_err_t fold_result(const bsp_epdinky_config_t *cfg, const char *what,
                             bool optional, esp_err_t err)
{
	if (err == ESP_OK) {
		return ESP_OK;
	}
	if (err == ESP_ERR_NOT_FOUND) {
		if (optional) {
			ESP_LOGW(TAG, "%s not populated - skipping", what);
			return ESP_OK;
		}
		if (cfg->allow_missing_required_devices) {
			ESP_LOGW(TAG, "%s is REQUIRED but not responding - continuing anyway "
			              "(allow_missing_required_devices is set)", what);
			return ESP_OK;
		}
		ESP_LOGE(TAG, "%s is required but did not respond", what);
		return err;
	}
	ESP_LOGE(TAG, "%s init failed: %s", what, esp_err_to_name(err));
	return err;
}

/* Optionality per the schematic: only the accelerometer and RTC are marked
 * OPTIONAL and may legitimately be absent. */
#define BSP_DEV_REQUIRED false
#define BSP_DEV_OPTIONAL true

esp_err_t bsp_epdinky_init_with_config(const bsp_epdinky_config_t *cfg,
                                       bsp_epdinky_handles_t *out)
{
	ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
	ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

	memset(out, 0, sizeof(*out));

	ESP_RETURN_ON_ERROR(bsp_i2c_init(&cfg->i2c), TAG, "i2c init failed");
	out->i2c_bus = s_i2c_bus;

	/* Required: gates SD card power and aggregates the sensor interrupts. */
	if (cfg->enable.use_tca6408) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "TCA6408", BSP_DEV_REQUIRED,
		                                bsp_tca6408_init(cfg, &out->tca6408)),
		                    TAG, "tca6408");
	}
	/* Park the panel bus before the PMIC can energise the rails, so the EPD is
	 * never driven by floating pins. */
	if (cfg->enable.use_epd_gpio) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "EPD GPIO", BSP_DEV_REQUIRED,
		                                bsp_epd_gpio_init(cfg)),
		                    TAG, "epd");
	}
	/* Required: without it there are no panel rails. */
	if (cfg->enable.use_tps65185) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "TPS65185", BSP_DEV_REQUIRED,
		                                bsp_tps65185_init(cfg, &out->tps65185)),
		                    TAG, "tps65185");
	}
	/* Marked OPTIONAL on the schematic. */
	if (cfg->enable.use_kxtj3) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "KXTJ3", BSP_DEV_OPTIONAL,
		                                bsp_kxtj3_init(cfg, &out->kxtj3)),
		                    TAG, "kxtj3");
	}
	/* Marked OPTIONAL on the schematic. */
	if (cfg->enable.use_rv3028) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "RV3028", BSP_DEV_OPTIONAL,
		                                bsp_rv3028_init(cfg, &out->rv3028)),
		                    TAG, "rv3028");
	}
	if (cfg->enable.use_stc3115) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "STC3115", BSP_DEV_REQUIRED,
		                                bsp_stc3115_init(cfg, &out->stc3115)),
		                    TAG, "stc3115");
	}
	if (cfg->enable.use_button) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "Button", BSP_DEV_REQUIRED,
		                                bsp_button_init(cfg)),
		                    TAG, "button");
	}
	if (cfg->enable.use_sdcard) {
		ESP_RETURN_ON_ERROR(fold_result(cfg, "SD card", BSP_DEV_REQUIRED,
		                                bsp_sdcard_mount(cfg, out->tca6408)),
		                    TAG, "sdcard");
	}

	return ESP_OK;
}

esp_err_t bsp_epdinky_init(bsp_epdinky_handles_t *out)
{
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	return bsp_epdinky_init_with_config(&cfg, out);
}

esp_err_t bsp_epdinky_init_default(bsp_epdinky_config_t *cfg, bsp_epdinky_handles_t *out)
{
	return bsp_epdinky_init_with_config(cfg, out);
}

esp_err_t bsp_epdinky_deinit(bsp_epdinky_handles_t *handles)
{
	ESP_RETURN_ON_FALSE(handles, ESP_ERR_INVALID_ARG, TAG, "handles is NULL");

	esp_err_t result = ESP_OK;

	/* Unmount and cut card power while the expander handle is still valid. */
	esp_err_t err = bsp_sdcard_unmount(handles->tca6408);
	if (err != ESP_OK) {
		result = err;
	}

	if (handles->tps65185) {
		/* Never leave the panel rails live behind a released handle. */
		tps65185_power_down(handles->tps65185);
		err = tps65185_deinit(handles->tps65185);
		if (err != ESP_OK) {
			result = err;
		}
	}
	if (handles->rv3028) {
		err = rv3028_deinit(handles->rv3028);
		if (err != ESP_OK) {
			result = err;
		}
	}
	if (handles->kxtj3) {
		err = kxtj3_delete(handles->kxtj3, handles->i2c_bus);
		if (err != ESP_OK) {
			result = err;
		}
	}
	if (handles->stc3115) {
		err = stc3115_delete(handles->stc3115);
		if (err != ESP_OK) {
			result = err;
		}
	}
	if (handles->tca6408) {
		err = tca6408_delete(handles->tca6408);
		if (err != ESP_OK) {
			result = err;
		}
	}

	s_button_io = GPIO_NUM_NC;
	memset(handles, 0, sizeof(*handles));

	/* Release the shared bus last: every device above lives on it. */
	err = bsp_i2c_deinit();
	if (err != ESP_OK) {
		result = err;
	}
	return result;
}

void bsp_epdinky_log_board_info(void)
{
	static const struct {
		uint8_t     addr;
		const char *name;
	} devices[] = {
		{ I2C_DEVICE_KXTJ3_ADDR,     "KXTJ3-1057  accelerometer" },
		{ I2C_DEVICE_KXTJ3_ADDR_ALT, "KXTJ3-1057  accelerometer (alt addr)" },
		{ I2C_DEVICE_TCA6408_ADDR,   "TCA6408A    GPIO expander" },
		{ I2C_DEVICE_RV3028_ADDR,    "RV-3028-C7  RTC" },
		{ I2C_DEVICE_TPS65185_ADDR,  "TPS651851   e-Ink PMIC" },
		{ I2C_DEVICE_STC3115_ADDR,   "STC3115     fuel gauge" },
	};

	ESP_LOGI(TAG, "epdInky ESP32-P4/C6 rev.2 - I2C inventory:");
	for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		bool present = (bsp_i2c_probe(devices[i].addr) == ESP_OK);
		ESP_LOGI(TAG, "  [%s] 0x%02x  %s",
		         present ? "found  " : "missing", devices[i].addr, devices[i].name);
	}
}
