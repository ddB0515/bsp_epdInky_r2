/*
 * epdInky ESP32-P4/C6 rev.2 - BSP bring-up demo.
 *
 * Shows both ways of using the BSP:
 *   1. bsp_epdinky_init_with_config() to bring up a chosen set of peripherals
 *   2. the individual bsp_<device>_init() calls, usable standalone
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp/epdinky_p4_board.h"

/* Real credentials live in wifi_credentials.h, which is gitignored. Copy
 * wifi_credentials.h.example to create it. Without it the demo still builds and
 * simply reports that no network is configured. */
#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#endif

static const char *TAG = "eInky-P4";

static void report_sensors(const bsp_epdinky_handles_t *board)
{
	if (board->kxtj3) {
		int16_t x, y, z;
		float x_mg, y_mg, z_mg;
		if (kxtj3_read_raw(board->kxtj3, &x, &y, &z) == ESP_OK &&
		    kxtj3_raw_to_mg(board->kxtj3, x, y, z, &x_mg, &y_mg, &z_mg) == ESP_OK) {
			ESP_LOGI(TAG, "Accel: X=%.0f mg  Y=%.0f mg  Z=%.0f mg", x_mg, y_mg, z_mg);
		}
	}

	if (board->rv3028) {
		rv3028_time_t now;
		if (rv3028_get_time(board->rv3028, &now) == ESP_OK) {
			ESP_LOGI(TAG, "RTC: %04u-%02u-%02u %02u:%02u:%02u",
			         now.year, now.month, now.date,
			         now.hours, now.minutes, now.seconds);
		}
	}

	if (board->stc3115) {
		stc3115_data_t batt;
		if (stc3115_read_data(board->stc3115, &batt) == ESP_OK) {
			ESP_LOGI(TAG, "Battery: %u mV  %ld uA  SoC %u.%u%%",
			         batt.voltage_mv, (long)batt.current_ua,
			         batt.soc_permille / 10u, batt.soc_permille % 10u);
		}
	}

	if (board->tca6408) {
		uint8_t inputs = 0;
		if (tca6408_get_input_val(board->tca6408, &inputs) == ESP_OK) {
			ESP_LOGI(TAG, "Expander inputs: 0x%02x (gsensor_int=%d rtc_int=%d)",
			         inputs,
			         (inputs >> BSP_TCA6408_PIN_GSENSOR_INT) & 1,
			         (inputs >> BSP_TCA6408_PIN_RTC_INT) & 1);
		}
	}
}

/*
 * Mount the card, write a text file, read it back and verify.
 *
 * Card power is switched by TCA6408 P7, so the expander handle is required.
 * The BSP keeps a boot counter in a second file to prove writes survive a
 * power cycle.
 */
static void test_sdcard(const bsp_epdinky_handles_t *board)
{
	ESP_LOGI(TAG, "--- SD card ---");

	if (!board->tca6408) {
		ESP_LOGE(TAG, "No expander handle - SD power cannot be switched");
		return;
	}

	esp_err_t err = bsp_sdcard_mount(NULL, board->tca6408);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
		ESP_LOGE(TAG, "Is a FAT-formatted card inserted?");
		return;
	}

	static const char *path = BSP_SD_MOUNT_POINT "/hello.txt";
	static const char *text = "Hello from epdInky ESP32-P4!\n";

	FILE *f = fopen(path, "w");
	if (!f) {
		ESP_LOGE(TAG, "fopen(%s, w) failed: %s", path, strerror(errno));
		bsp_sdcard_unmount(board->tca6408);
		return;
	}
	fprintf(f, "%s", text);
	fprintf(f, "Built %s %s\n", __DATE__, __TIME__);
	fclose(f);
	ESP_LOGI(TAG, "Wrote %s", path);

	/* Read it back and compare against what we just wrote. */
	char buf[128] = {0};
	f = fopen(path, "r");
	if (!f) {
		ESP_LOGE(TAG, "fopen(%s, r) failed: %s", path, strerror(errno));
		bsp_sdcard_unmount(board->tca6408);
		return;
	}
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	if (strncmp(buf, text, strlen(text)) == 0) {
		ESP_LOGI(TAG, "Read back OK (%u bytes):", (unsigned)n);
		printf("---8<--- %s ---\n%s---8<--- end ---\n", path, buf);
	} else {
		ESP_LOGE(TAG, "Read back MISMATCH, got: %s", buf);
	}

	/* Append-mode test: a boot counter that must survive power cycles. */
	static const char *cnt_path = BSP_SD_MOUNT_POINT "/boots.txt";
	unsigned boots = 0;
	f = fopen(cnt_path, "r");
	if (f) {
		if (fscanf(f, "%u", &boots) != 1) {
			boots = 0;
		}
		fclose(f);
	}
	boots++;
	f = fopen(cnt_path, "w");
	if (f) {
		fprintf(f, "%u\n", boots);
		fclose(f);
		ESP_LOGI(TAG, "Boot count on card: %u", boots);
	} else {
		ESP_LOGW(TAG, "Could not update %s: %s", cnt_path, strerror(errno));
	}

	/* Show what is on the card. */
	DIR *dir = opendir(BSP_SD_MOUNT_POINT);
	if (dir) {
		ESP_LOGI(TAG, "Contents of %s:", BSP_SD_MOUNT_POINT);
		struct dirent *e;
		while ((e = readdir(dir)) != NULL) {
			struct stat st;
			char full[280];
			snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, e->d_name);
			if (stat(full, &st) == 0) {
				printf("    %-20s %8ld bytes\n", e->d_name, (long)st.st_size);
			} else {
				printf("    %-20s\n", e->d_name);
			}
		}
		closedir(dir);
	}

	/* Leave the card mounted so the app can keep using it. Call
	 * bsp_sdcard_unmount(board->tca6408) to power it back down. */
	ESP_LOGI(TAG, "SD card test complete (left mounted at %s)", BSP_SD_MOUNT_POINT);
}

/* Re-read the test file to prove the card still works while the C6 SDIO link
 * is active. Both share one SDMMC controller, so this is worth checking. */
static bool sdcard_still_readable(void)
{
	FILE *f = fopen(BSP_SD_MOUNT_POINT "/hello.txt", "r");
	if (!f) {
		return false;
	}
	char buf[64] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	return n > 0 && strncmp(buf, "Hello from epdInky", 18) == 0;
}

/*
 * Button-driven Wi-Fi demo: the board starts the radio at boot but stays
 * offline until SW4 is pressed. Pressing again drops the connection, so the
 * same button both joins and leaves the network.
 */
static void demo_toggle_wifi(void)
{
#ifndef DEMO_WIFI_SSID
	ESP_LOGW(TAG, "Button pressed, but no Wi-Fi credentials are configured.");
	ESP_LOGW(TAG, "Copy main/wifi_credentials.h.example to main/wifi_credentials.h.");
#else
	if (bsp_wifi_is_connected()) {
		ESP_LOGI(TAG, "Button pressed - leaving \"%s\"", DEMO_WIFI_SSID);
		esp_err_t err = bsp_wifi_disconnect();
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Disconnect failed: %s", esp_err_to_name(err));
		} else {
			ESP_LOGI(TAG, "Disconnected");
		}
		return;
	}

	ESP_LOGI(TAG, "Button pressed - joining \"%s\"...", DEMO_WIFI_SSID);
	esp_err_t err = bsp_wifi_connect(DEMO_WIFI_SSID, DEMO_WIFI_PASSWORD, 20000);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Could not join \"%s\": %s", DEMO_WIFI_SSID, esp_err_to_name(err));
		/* Stop the background retry loop so a bad password does not keep the
		 * radio busy until the next press. */
		bsp_wifi_disconnect();
		return;
	}

	char ip[16];
	if (bsp_wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK) {
		ESP_LOGI(TAG, "Online as %s - press again to disconnect", ip);
	}
#endif
}

void app_main(void)
{
	ESP_LOGI(TAG, "epdInky ESP32-P4/C6 rev.2 starting");

	/* Pick exactly what to bring up. Everything defaults to enabled except
	 * the SD card; the PMIC rails stay off until explicitly requested. */
	bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
	cfg.enable.use_tca6408  = true;
	cfg.enable.use_tps65185 = true;
	cfg.enable.use_kxtj3    = true;
	cfg.enable.use_rv3028   = true;
	cfg.enable.use_stc3115  = true;
	cfg.enable.use_epd_gpio = true;
	cfg.enable.use_button   = true;
	cfg.enable.use_sdcard   = false;   /* needs the expander for card power */

	/* The accelerometer and RTC are marked OPTIONAL on the schematic, so the
	 * BSP already tolerates them being depopulated. Everything else is
	 * required and will fail init if it does not respond. */

	bsp_epdinky_handles_t board;
	esp_err_t err = bsp_epdinky_init_with_config(&cfg, &board);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
		return;
	}

	ESP_LOGI(TAG, "--- I2C bus scan ---");
	bsp_i2c_scan();
	bsp_epdinky_log_board_info();

	test_sdcard(&board);

	/* Wi-Fi lives on the ESP32-C6 and is reached over SDIO via esp-hosted.
	 * This call resets the C6, so it takes a moment the first time.
	 * The radio is started here but deliberately does NOT join a network -
	 * that happens only on a button press below. */
	ESP_LOGI(TAG, "--- Wi-Fi ---");
	err = bsp_wifi_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
		ESP_LOGE(TAG, "Check the ESP32-C6 is flashed with matching esp-hosted slave firmware.");
	} else {
		bsp_wifi_scan_print();
#ifdef DEMO_WIFI_SSID
		ESP_LOGI(TAG, "Press the user button (SW4) to join \"%s\"", DEMO_WIFI_SSID);
#else
		ESP_LOGW(TAG, "No credentials configured - copy "
		              "main/wifi_credentials.h.example to main/wifi_credentials.h");
#endif
	}

	/* BLE shares the same esp-hosted link: the NimBLE host runs here on the P4
	 * while the controller lives on the C6. */
	ESP_LOGI(TAG, "--- BLE ---");
	err = bsp_ble_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
	} else {
		bsp_ble_scan_print(5000);
	}

	bool was_pressed = false;
	unsigned tick = 0;
	while (1) {
		// report_sensors(&board);

		/* Every 10s, confirm SD and Wi-Fi coexist on the shared SDMMC controller. */
		if (++tick % 10 == 0) {
			char ip[16];
			bool online = bsp_wifi_get_ip_str(ip, sizeof(ip)) == ESP_OK;
			ESP_LOGI(TAG, "Coexistence check: SD readable=%s, Wi-Fi=%s",
			         sdcard_still_readable() ? "yes" : "NO",
			         online ? ip : "up (not joined)");
		}

		/* Act on the press edge so holding the button does not retrigger. */
		bool pressed = bsp_button_is_pressed();
		if (pressed && !was_pressed) {
			demo_toggle_wifi();
		}
		was_pressed = pressed;

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
