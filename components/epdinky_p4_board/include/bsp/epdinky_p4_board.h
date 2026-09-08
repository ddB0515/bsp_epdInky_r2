#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "config.h"
#include "tca6408.h"
#include "tps65185.h"
#include "stc3115.h"
#include "rv3028.h"
#include "kxtj3_1057.h"

#define BSP_BOARD_EPDINKY_P4_BOARD

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Configuration
 * ========================================================================= */

typedef struct {
	i2c_port_t port;
	int        sda_io;
	int        scl_io;
	uint32_t   freq_hz;
	bool       enable_pullups;   /* keep false: board has external 2K2 pull-ups */
} bsp_epdinky_i2c_cfg_t;

typedef struct {
	gpio_num_t pwrgood_io;
	gpio_num_t power_up_io;
	gpio_num_t int_io;
	gpio_num_t vcom_ctrl_io;
	gpio_num_t wake_up_io;
} bsp_epdinky_tps65185_gpio_t;

typedef struct {
	gpio_num_t int_io;           /* aggregate expander interrupt, active low */
} bsp_epdinky_tca6408_gpio_t;

typedef struct {
	gpio_num_t data_io[BSP_EPD_DATA_PIN_COUNT];
	gpio_num_t spv_io;
	gpio_num_t ckv_io;
	gpio_num_t mode_io;
	gpio_num_t xstl_io;
	gpio_num_t xoe_io;
	gpio_num_t xle_io;
	gpio_num_t xcl_io;
} bsp_epdinky_epd_gpio_t;

typedef struct {
	gpio_num_t cmd_io;
	gpio_num_t clk_io;
	gpio_num_t d0_io;
	gpio_num_t d1_io;
	gpio_num_t d2_io;
	gpio_num_t d3_io;
	int        slot;
	/* Card power is switched by TCA6408 P7 (active low), not by a P4 GPIO. */
	int        power_expander_pin;
} bsp_epdinky_sd_cfg_t;

typedef struct {
	bsp_epdinky_tps65185_gpio_t tps65185;
	bsp_epdinky_tca6408_gpio_t  tca6408;
	bsp_epdinky_epd_gpio_t      epd;
	bsp_epdinky_sd_cfg_t        sd;
	gpio_num_t                  button_io;
} bsp_epdinky_gpio_cfg_t;

typedef struct {
	uint8_t kxtj3;
	uint8_t kxtj3_alt;   /* probed if the primary address does not answer */
	uint8_t rv3028;
	uint8_t tps65185;
	uint8_t tca6408;
	uint8_t stc3115;
} bsp_epdinky_i2c_addrs_t;

typedef struct {
	int  kxtj3_range_g;
	int  kxtj3_odr_hz;
	bool rv3028_trickle_chg_en;
	int  stc3115_capacity_mAh;
	int  stc3115_sense_mohm;
	/* TPS65185: rails are NOT powered during init unless this is set. */
	bool tps65185_power_up_rails;
	int  tps65185_vcom_mV;       /* magnitude in mV, e.g. 1500 => -1.5 V */
	int  tps65185_vpos_vneg_mV;  /* e.g. 15000 for +/-15 V */
} bsp_epdinky_device_opts_t;

typedef struct {
	bool use_tca6408;
	bool use_tps65185;
	bool use_stc3115;
	bool use_kxtj3;
	bool use_rv3028;
	bool use_epd_gpio;
	bool use_button;
	bool use_sdcard;
} bsp_epdinky_devices_t;

typedef struct {
	bsp_epdinky_i2c_cfg_t     i2c;
	bsp_epdinky_gpio_cfg_t    gpio;
	bsp_epdinky_i2c_addrs_t   addrs;
	bsp_epdinky_device_opts_t opts;
	bsp_epdinky_devices_t     enable;
	/*
	 * Which devices may legitimately be absent is a property of the board, not
	 * of the caller, so it is not configurable: the schematic marks only the
	 * KXTJ3 accelerometer and RV-3028 RTC as OPTIONAL. Those are always
	 * downgraded to a warning if they do not respond; every other device is
	 * required and its absence fails bsp_epdinky_init_with_config().
	 *
	 * Set this only for bench bring-up on a partially populated board: it
	 * downgrades a missing *required* device to a warning too, which will hand
	 * you NULL handles instead of an error. Leave it false in production.
	 */
	bool allow_missing_required_devices;
} bsp_epdinky_config_t;

typedef struct {
	i2c_master_bus_handle_t i2c_bus;
	tca6408_handle_t        tca6408;
	tps65185_handle_t       tps65185;
	stc3115_handle_t        stc3115;
	kxtj3_handle_t          kxtj3;
	rv3028_handle_t         rv3028;
} bsp_epdinky_handles_t;

/* ===========================================================================
 * Core: I2C bus
 *
 * Every bsp_*_init() below brings the bus up on demand, so components can be
 * initialised individually and in any order.
 * ========================================================================= */

/** @brief Populate a config with the verified board defaults. */
bsp_epdinky_config_t bsp_epdinky_default_config(void);

/** @brief Bring up the shared I2C bus. Idempotent. */
esp_err_t bsp_i2c_init(const bsp_epdinky_i2c_cfg_t *cfg);

/** @brief Get the shared bus handle, initialising it with defaults if needed. */
esp_err_t bsp_i2c_get_handle(i2c_master_bus_handle_t *out);

/** @brief Tear down the shared I2C bus. */
esp_err_t bsp_i2c_deinit(void);

/** @brief Probe a 7-bit address on the shared bus. */
esp_err_t bsp_i2c_probe(uint8_t addr);

/** @brief Print an i2cdetect-style map of the shared bus. */
esp_err_t bsp_i2c_scan(void);

/* ===========================================================================
 * Individual component init. Each is independent and safe to call alone.
 * Returns ESP_ERR_NOT_FOUND if the device does not acknowledge on I2C.
 * ========================================================================= */

/** @brief TCA6408A expander (0x21). Sets SD power gate off, sensor pins input. */
esp_err_t bsp_tca6408_init(const bsp_epdinky_config_t *cfg, tca6408_handle_t *out);

/**
 * @brief TPS65185 e-Ink PMIC (0x68).
 * Rails stay off unless cfg->opts.tps65185_power_up_rails is set.
 */
esp_err_t bsp_tps65185_init(const bsp_epdinky_config_t *cfg, tps65185_handle_t *out);

/** @brief KXTJ3-1057 accelerometer. Tries the primary then the JP2 alternate address. */
esp_err_t bsp_kxtj3_init(const bsp_epdinky_config_t *cfg, kxtj3_handle_t *out);

/** @brief RV-3028-C7 RTC (0x52). */
esp_err_t bsp_rv3028_init(const bsp_epdinky_config_t *cfg, rv3028_handle_t *out);

/** @brief STC3115 fuel gauge (0x70). No alert line to the MCU - poll it. */
esp_err_t bsp_stc3115_init(const bsp_epdinky_config_t *cfg, stc3115_handle_t *out);

/** @brief Park the EPD parallel bus and control lines in a safe, idle state. */
esp_err_t bsp_epd_gpio_init(const bsp_epdinky_config_t *cfg);

/** @brief Configure the SW4 user button (GPIO35, active low). */
esp_err_t bsp_button_init(const bsp_epdinky_config_t *cfg);

/** @brief True while SW4 is held down. */
bool bsp_button_is_pressed(void);

/* ===========================================================================
 * SD card. Power is gated through the expander, so a valid TCA6408 handle is
 * required before the slot can be used.
 * ========================================================================= */

/** @brief Switch micro-SD card power via TCA6408 P7 (active low). */
esp_err_t bsp_sdcard_set_power(tca6408_handle_t tca, bool on);

/** @brief Power and mount the card at BSP_SD_MOUNT_POINT. */
esp_err_t bsp_sdcard_mount(const bsp_epdinky_config_t *cfg, tca6408_handle_t tca);

/** @brief Unmount and power the card down. */
esp_err_t bsp_sdcard_unmount(tca6408_handle_t tca);

/* ===========================================================================
 * Wi-Fi (ESP32-C6 over SDIO via esp-hosted)
 *
 * The radio is a separate chip reached over SDIO slot 1. Its EN line (GPIO54)
 * is driven by esp-hosted itself - the BSP never touches it.
 * ========================================================================= */

/** @brief One access point returned by bsp_wifi_scan(). */
typedef struct {
	char             ssid[33];
	uint8_t          bssid[6];
	int8_t           rssi;
	uint8_t          channel;
	wifi_auth_mode_t authmode;
} bsp_wifi_ap_t;

/**
 * @brief Bring up NVS, the network stack and the remote Wi-Fi radio in
 *        station mode. Idempotent.
 *
 * This is the point where the ESP32-C6 is reset and the esp-hosted transport is
 * negotiated, so it takes a moment and will fail if the C6 is not running
 * matching esp-hosted slave firmware.
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief Run a blocking scan for access points.
 *
 * @param out    Destination array (may be NULL to only count).
 * @param max    Capacity of @p out.
 * @param found  Receives the number of APs written (or total seen if out==NULL).
 */
esp_err_t bsp_wifi_scan(bsp_wifi_ap_t *out, size_t max, size_t *found);

/** @brief Scan and print the results as a table. */
esp_err_t bsp_wifi_scan_print(void);

/** @brief Join a network and wait for an IPv4 address. */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/** @brief Leave the current network. */
esp_err_t bsp_wifi_disconnect(void);

/** @brief True once the station has an IPv4 address. */
bool bsp_wifi_is_connected(void);

/**
 * @brief Get the station's IPv4 address as a dotted-quad string.
 *
 * @param out  Destination buffer; 16 bytes is enough for "255.255.255.255".
 * @param len  Size of @p out.
 * @return ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t bsp_wifi_get_ip_str(char *out, size_t len);

/** @brief Human-readable name for a wifi_auth_mode_t. */
const char *bsp_wifi_authmode_str(wifi_auth_mode_t mode);

/** @brief Stop and release the Wi-Fi stack. */
esp_err_t bsp_wifi_deinit(void);

/**
 * @brief  Start a SoftAP alongside the (idle) station interface.
 *
 * WIFI_MODE_APSTA, not pure AP: esp_wifi_scan_start() only works with a
 * station interface present, and a provisioning page that lists nearby
 * networks needs to be able to scan while its own AP is up. There is nothing
 * for the station side to be connected to during provisioning, so this is
 * "AP with an idle STA alongside it" in practice.
 *
 * The AP gets a fixed IP, 4.3.2.1/24, rather than the usual 192.168.4.1 -
 * deliberately: this mirrors the upstream firmware this AP exists to
 * provision, in case anything on the client side ever comes to depend on the
 * address rather than following the gateway it's handed.
 *
 * @param ssid      Up to 32 bytes.
 * @param password  NULL or empty for an open network.
 * @param channel   0 lets the driver pick.
 */
esp_err_t bsp_wifi_ap_start(const char *ssid, const char *password, uint8_t channel);

/** @brief Stop the SoftAP and return to WIFI_MODE_STA. */
esp_err_t bsp_wifi_ap_stop(void);

/**
 * @brief  Get the SoftAP's own IPv4 address as a dotted-quad string.
 *
 * Reads back the address bsp_wifi_ap_start() configured rather than assuming
 * the caller remembers it, so there is one place that knows it.
 *
 * @param out  Destination buffer; 16 bytes is enough for "255.255.255.255".
 * @param len  Size of @p out.
 * @return ESP_ERR_INVALID_STATE if the AP is not running.
 */
esp_err_t bsp_wifi_ap_get_ip_str(char *out, size_t len);

/* ===========================================================================
 * Bluetooth LE (NimBLE host on the P4, controller on the ESP32-C6)
 *
 * The C6 reports "BLE only" over esp-hosted - classic Bluetooth is not
 * available. BLE and Wi-Fi share the same SDIO link.
 * ========================================================================= */

#define BSP_BLE_SCAN_MAX 32

/** @brief One device seen during a BLE scan. */
typedef struct {
	uint8_t addr[6];      /**< little-endian, as NimBLE stores it */
	uint8_t addr_type;    /**< BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM  */
	int8_t  rssi;
	char    name[32];     /**< empty if the device advertised no name */
} bsp_ble_device_t;

/**
 * @brief Start the NimBLE host and wait for the remote controller to sync.
 *
 * Idempotent. Requires the esp-hosted transport, so it will fail if the C6 is
 * not running matching slave firmware.
 */
esp_err_t bsp_ble_init(void);

/**
 * @brief Run a blocking BLE scan.
 *
 * Duplicate advertisements are merged, so each device appears once.
 *
 * @param out          Destination array.
 * @param max          Capacity of @p out.
 * @param found        Receives the number of distinct devices seen.
 * @param duration_ms  Scan window; 0 selects 5000 ms.
 */
esp_err_t bsp_ble_scan(bsp_ble_device_t *out, size_t max, size_t *found,
                       uint32_t duration_ms);

/** @brief Scan and print the results as a table. */
esp_err_t bsp_ble_scan_print(uint32_t duration_ms);

/** @brief True once the NimBLE host has synced with the controller. */
bool bsp_ble_is_ready(void);

/** @brief Stop and release the NimBLE host. */
esp_err_t bsp_ble_deinit(void);

/* ===========================================================================
 * Whole-board init
 * ========================================================================= */

/** @brief Init every component enabled in the default config. */
esp_err_t bsp_epdinky_init(bsp_epdinky_handles_t *out);

/** @brief Init exactly the components enabled in @p cfg. */
esp_err_t bsp_epdinky_init_with_config(const bsp_epdinky_config_t *cfg,
                                       bsp_epdinky_handles_t *out);

/** @brief Release every handle produced by a board init. */
esp_err_t bsp_epdinky_deinit(bsp_epdinky_handles_t *handles);

/** @brief Log which peripherals are present on the bus. */
void bsp_epdinky_log_board_info(void);

/* Backwards-compatible alias for the previous entry point. */
esp_err_t bsp_epdinky_init_default(bsp_epdinky_config_t *cfg, bsp_epdinky_handles_t *out);

#ifdef __cplusplus
}
#endif
