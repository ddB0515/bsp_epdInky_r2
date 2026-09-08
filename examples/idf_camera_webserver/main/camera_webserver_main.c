/*
 * epdInky ESP32-P4/C6 - MIPI CSI camera web server.
 *
 * Brings up the board, joins Wi-Fi through the on-board ESP32-C6, starts the
 * SC2336 camera and serves:
 *
 *   - an MJPEG preview over HTTP, with live controls and snapshot-to-SD, and
 *   - four H.264 RTSP streams, one per sensor resolution.
 *
 * Wi-Fi credentials live in main/wifi_credentials.h (gitignored); copy
 * wifi_credentials.h.example to create it.
 */

#include <inttypes.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/epdinky_p4_board.h"

#include "app_camera.h"
#include "app_httpd.h"
#include "app_power.h"
#include "app_snapshot.h"
#include "rtsp_server.h"

#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#endif

static const char *TAG = "camera_web";


/* Seconds without a viewer, an RTSP session or a web request before sleeping. */
#define IDLE_TIMEOUT_S 60

static bsp_epdinky_config_t  s_board_cfg;
static bsp_epdinky_handles_t s_board;

#define RESUME_STEP(name, expr)                                        \
	do {                                                               \
		esp_err_t _e = (expr);                                         \
		if (_e != ESP_OK) {                                            \
			app_power_record_failure((name), _e);                      \
			ESP_LOGE(TAG, "%s failed: %s", (name), esp_err_to_name(_e)); \
			return _e;                                                 \
		}                                                              \
	} while (0)

/*
 * Bring the board into service.
 *
 * Also used to come back after a nap, so every step has to be safe to run more
 * than once: the matching tear_down() releases everything this claims.
 */
static esp_err_t bring_up(void)
{
	RESUME_STEP("wifi_init", bsp_wifi_init());

	ESP_LOGI(TAG, "Joining \"%s\"...", DEMO_WIFI_SSID);
	RESUME_STEP("wifi_connect",
	            bsp_wifi_connect(DEMO_WIFI_SSID, DEMO_WIFI_PASSWORD, 30000));

	/* Now that esp-hosted owns the SDMMC controller, the card can share it.
	 * A missing card is not fatal - snapshots are simply unavailable.
	 *
	 * Retried once because after a sleep the card has just been power-cycled
	 * and does not always answer the first initialisation attempt. */
	esp_err_t sd_err = bsp_sdcard_mount(&s_board_cfg, s_board.tca6408);
	if (sd_err != ESP_OK) {
		vTaskDelay(pdMS_TO_TICKS(250));
		sd_err = bsp_sdcard_mount(&s_board_cfg, s_board.tca6408);
	}
	if (sd_err != ESP_OK) {
		ESP_LOGW(TAG, "No SD card (%s); snapshots will be unavailable",
		         esp_err_to_name(sd_err));
	}

	/* Start the camera only once the network is up, so a sensor problem is
	 * easy to tell apart from a Wi-Fi problem in the log. */
	RESUME_STEP("camera_init", app_camera_init());
	RESUME_STEP("snapshot_init", app_snapshot_init());
	RESUME_STEP("httpd_start", app_httpd_start());
	RESUME_STEP("rtsp_start", rtsp_server_start());

	char ip[16] = "?";
	bsp_wifi_get_ip_str(ip, sizeof(ip));

	uint32_t width = 0, height = 0;
	app_camera_get_size(&width, &height);

	ESP_LOGI(TAG, "=====================================================");
	ESP_LOGI(TAG, " Camera ready at %" PRIu32 "x%" PRIu32, width, height);
	ESP_LOGI(TAG, " Preview  http://%s/  (press \"Open stream\")", ip);
	ESP_LOGI(TAG, " RTSP     rtsp://%s:%d/stream1  H.264 1920x1080", ip, RTSP_SERVER_PORT);
	ESP_LOGI(TAG, "          rtsp://%s:%d/stream2  H.264 1280x720",  ip, RTSP_SERVER_PORT);
	ESP_LOGI(TAG, "          rtsp://%s:%d/stream3  H.264 800x800",   ip, RTSP_SERVER_PORT);
	ESP_LOGI(TAG, "          rtsp://%s:%d/stream4  H.264 640x480",   ip, RTSP_SERVER_PORT);
	if (app_snapshot_sd_available()) {
		ESP_LOGI(TAG, " Snapshots save to %s/snapNNNN.jpg", BSP_SD_MOUNT_POINT);
	}
	ESP_LOGI(TAG, " Idle for %d s with nobody connected -> sleep", IDLE_TIMEOUT_S);
	ESP_LOGI(TAG, "=====================================================");
	return ESP_OK;
}

/*
 * Release everything bring_up() claimed, in the reverse order.
 *
 * The servers go first so no new client can arrive mid-teardown, then the
 * camera (which powers the sensor down), then the card, then the radio.
 */
static void tear_down(void)
{
	rtsp_server_stop();
	app_httpd_stop();
	app_camera_suspend();
	bsp_wifi_deinit();

	/* The SD card is deliberately left mounted. Powering it down saves very
	 * little - it is idle anyway - and the card does not reliably answer
	 * initialisation again after being power-cycled, which left snapshots
	 * broken for the rest of the session. Keeping it mounted is the better
	 * trade. */
	ESP_LOGI(TAG, "Camera and Wi-Fi are down");
}

void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4 camera web server");

	esp_err_t   prev_err  = ESP_OK;
	const char *prev_step = app_power_last_failure(&prev_err);
	if (prev_step[0]) {
		ESP_LOGW(TAG, "Previous resume failed at %s (%s) - restarted",
		         prev_step, esp_err_to_name(prev_err));
	}

	/* The camera needs the I2C bus (for SCCB), and snapshots need the SD card.
	 * The card's power is gated behind the TCA6408 expander, so that has to
	 * come up too. Everything else stays out of the way.
	 *
	 * The card is mounted further down rather than here: the P4 has a single
	 * SDMMC controller and esp-hosted claims it for the C6 radio, so Wi-Fi has
	 * to come up first. Mounting it here would also make a missing card fatal,
	 * and the camera should still work without one. */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tps65185 = false;
	cfg.enable.use_kxtj3    = false;
	cfg.enable.use_rv3028   = false;
	cfg.enable.use_stc3115  = false;
	cfg.enable.use_epd_gpio = false;
	cfg.enable.use_tca6408  = true;
	cfg.enable.use_sdcard   = false;

	bsp_epdinky_handles_t board;
	ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &board));

#ifndef DEMO_WIFI_SSID
	ESP_LOGE(TAG, "No Wi-Fi credentials configured.");
	ESP_LOGE(TAG, "Copy main/wifi_credentials.h.example to main/wifi_credentials.h.");
	return;
#else
	s_board_cfg = cfg;
	s_board     = board;

	if (bring_up() != ESP_OK) {
		return;
	}

	/* Sleep once nothing has used the board for a while. */
	static const app_power_hooks_t power_hooks = {
		.suspend = tear_down,
		.resume  = bring_up,
	};
	ESP_ERROR_CHECK(app_power_start(&power_hooks, IDLE_TIMEOUT_S));

	while (true) {
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
#endif
}
