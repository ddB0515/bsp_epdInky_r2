/*
 * Wi-Fi support for the epdInky ESP32-P4/C6 board.
 *
 * The P4 has no radio of its own. Wi-Fi is provided by the on-board
 * ESP32-C6-MINI-1 reached over SDIO slot 1, with esp-hosted transporting the
 * standard esp_wifi API across the link. From the application's point of view
 * the normal esp_wifi_* calls apply; the only board-specific concern is that
 * esp-hosted owns the C6 EN line (GPIO54) and resets the radio during
 * esp_wifi_init().
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "bsp/epdinky_p4_board.h"

static const char *TAG = "epdinky_wifi";

#define BSP_WIFI_CONNECTED_BIT BIT0
#define BSP_WIFI_FAIL_BIT      BIT1
#define BSP_WIFI_SCAN_MAX      32

static bool                s_wifi_started;
static esp_netif_t        *s_sta_netif;
static esp_netif_t        *s_ap_netif;
static EventGroupHandle_t  s_wifi_events;
static esp_event_handler_instance_t s_any_id_handler;
static esp_event_handler_instance_t s_got_ip_handler;
static bool                s_connect_requested;

/* 4.3.2.1/24 - see bsp_wifi_ap_start()'s doc comment for why this is not the
 * usual 192.168.4.1. */
#define BSP_WIFI_AP_IP_A 4
#define BSP_WIFI_AP_IP_B 3
#define BSP_WIFI_AP_IP_C 2
#define BSP_WIFI_AP_IP_D 1

/* ===========================================================================
 * Events
 * ========================================================================= */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		if (s_connect_requested) {
			esp_wifi_connect();
		}
		return;
	}

	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *ev = data;
		xEventGroupClearBits(s_wifi_events, BSP_WIFI_CONNECTED_BIT);
		if (s_connect_requested) {
			ESP_LOGW(TAG, "Disconnected (reason %d), retrying", ev ? ev->reason : 0);
			/* Surface the failure to a waiting bsp_wifi_connect() but keep
			 * retrying in the background. */
			xEventGroupSetBits(s_wifi_events, BSP_WIFI_FAIL_BIT);
			esp_wifi_connect();
		}
		return;
	}

	if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *ev = data;
		ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
		xEventGroupClearBits(s_wifi_events, BSP_WIFI_FAIL_BIT);
		xEventGroupSetBits(s_wifi_events, BSP_WIFI_CONNECTED_BIT);
	}
}

/* ===========================================================================
 * Init / deinit
 * ========================================================================= */

esp_err_t bsp_wifi_init(void)
{
	if (s_wifi_started) {
		return ESP_OK;
	}

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
		err = nvs_flash_init();
	}
	ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

	ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_RETURN_ON_ERROR(err, TAG, "event loop failed");
	}

	if (!s_sta_netif) {
		s_sta_netif = esp_netif_create_default_wifi_sta();
		ESP_RETURN_ON_FALSE(s_sta_netif, ESP_FAIL, TAG, "failed to create STA netif");
	}

	if (!s_wifi_events) {
		s_wifi_events = xEventGroupCreate();
		ESP_RETURN_ON_FALSE(s_wifi_events, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
	}

	/* This is where esp-hosted resets the C6 and negotiates the SDIO
	 * transport, so it is the call that fails if the slave firmware is
	 * missing or mismatched. */
	ESP_LOGI(TAG, "Starting Wi-Fi via ESP32-C6 (esp-hosted over SDIO slot %d)",
	         BSP_C6_WIFI_SDIO_SLOT);

	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG,
	                    "esp_wifi_init failed - is the C6 running esp-hosted slave firmware?");

	ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
	                        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
	                        NULL, &s_any_id_handler),
	                    TAG, "wifi event register failed");
	ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
	                        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
	                        NULL, &s_got_ip_handler),
	                    TAG, "ip event register failed");

	ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
	ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

	s_wifi_started = true;

	uint8_t mac[6] = {0};
	if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
		ESP_LOGI(TAG, "Wi-Fi ready, STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}
	return ESP_OK;
}

esp_err_t bsp_wifi_deinit(void)
{
	if (!s_wifi_started) {
		return ESP_OK;
	}

	s_connect_requested = false;
	esp_wifi_disconnect();
	esp_err_t err = esp_wifi_stop();

	if (s_any_id_handler) {
		esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_any_id_handler);
		s_any_id_handler = NULL;
	}
	if (s_got_ip_handler) {
		esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_got_ip_handler);
		s_got_ip_handler = NULL;
	}

	esp_wifi_deinit();

	if (s_sta_netif) {
		esp_netif_destroy_default_wifi(s_sta_netif);
		s_sta_netif = NULL;
	}
	if (s_wifi_events) {
		vEventGroupDelete(s_wifi_events);
		s_wifi_events = NULL;
	}

	s_wifi_started = false;
	return err;
}

/* ===========================================================================
 * SoftAP (provisioning)
 * ========================================================================= */

esp_err_t bsp_wifi_ap_start(const char *ssid, const char *password, uint8_t channel)
{
	ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid is empty");
	ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "wifi not available");

	/*
	 * Stop first rather than switching mode live. This board's Wi-Fi is a
	 * remote radio reached over esp-hosted, not local silicon, and a mode
	 * change is exactly the kind of thing that needs to be unsurprising on a
	 * transport nobody has exercised in AP mode yet - see the doc comment on
	 * bsp_wifi_ap_start() in the header.
	 *
	 * APSTA, not pure AP: esp_wifi_scan_start() requires a station interface
	 * to exist, so a provisioning page that wants to list nearby networks
	 * needs the AP to come up alongside STA rather than replacing it.
	 */
	ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "stop failed");

	if (!s_ap_netif) {
		s_ap_netif = esp_netif_create_default_wifi_ap();
		ESP_RETURN_ON_FALSE(s_ap_netif, ESP_FAIL, TAG, "failed to create AP netif");
	}

	esp_netif_ip_info_t ip_info = {
		.ip      = { .addr = ESP_IP4TOADDR(BSP_WIFI_AP_IP_A, BSP_WIFI_AP_IP_B,
		                                    BSP_WIFI_AP_IP_C, BSP_WIFI_AP_IP_D) },
		.gw      = { .addr = ESP_IP4TOADDR(BSP_WIFI_AP_IP_A, BSP_WIFI_AP_IP_B,
		                                    BSP_WIFI_AP_IP_C, BSP_WIFI_AP_IP_D) },
		.netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
	};
	ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_ap_netif), TAG, "dhcps stop failed");
	ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "set ip failed");
	ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "dhcps start failed");

	wifi_config_t ap_cfg = { 0 };
	strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
	ap_cfg.ap.ssid_len = strlen(ssid);
	ap_cfg.ap.channel  = channel;
	ap_cfg.ap.max_connection = 4;
	if (password && password[0]) {
		strncpy((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password) - 1);
		ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
	} else {
		ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode failed");
	ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set ap config failed");
	ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

	ESP_LOGI(TAG, "SoftAP \"%s\" up (%s), http://%d.%d.%d.%d/",
	         ssid, ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2",
	         BSP_WIFI_AP_IP_A, BSP_WIFI_AP_IP_B, BSP_WIFI_AP_IP_C, BSP_WIFI_AP_IP_D);
	return ESP_OK;
}

esp_err_t bsp_wifi_ap_stop(void)
{
	if (!s_ap_netif) {
		return ESP_OK;
	}

	esp_err_t err = esp_wifi_stop();
	esp_netif_destroy_default_wifi(s_ap_netif);
	s_ap_netif = NULL;

	ESP_RETURN_ON_ERROR(err, TAG, "stop failed");
	ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
	return esp_wifi_start();
}

esp_err_t bsp_wifi_ap_get_ip_str(char *out, size_t len)
{
	ESP_RETURN_ON_FALSE(out && len >= 16, ESP_ERR_INVALID_ARG, TAG, "buffer too small");
	ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_INVALID_STATE, TAG, "AP not started");

	esp_netif_ip_info_t ip_info;
	ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_ap_netif, &ip_info), TAG, "no ip info");

	snprintf(out, len, IPSTR, IP2STR(&ip_info.ip));
	return ESP_OK;
}

/* ===========================================================================
 * Scanning
 * ========================================================================= */

const char *bsp_wifi_authmode_str(wifi_auth_mode_t mode)
{
	switch (mode) {
	case WIFI_AUTH_OPEN:            return "open";
	case WIFI_AUTH_WEP:             return "WEP";
	case WIFI_AUTH_WPA_PSK:         return "WPA";
	case WIFI_AUTH_WPA2_PSK:        return "WPA2";
	case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
	case WIFI_AUTH_ENTERPRISE:      return "WPA2-ENT";
	case WIFI_AUTH_WPA3_PSK:        return "WPA3";
	case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
	case WIFI_AUTH_WAPI_PSK:        return "WAPI";
	case WIFI_AUTH_OWE:             return "OWE";
	default:                        return "unknown";
	}
}

esp_err_t bsp_wifi_scan(bsp_wifi_ap_t *out, size_t max, size_t *found)
{
	ESP_RETURN_ON_FALSE(found, ESP_ERR_INVALID_ARG, TAG, "found is NULL");
	ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "wifi not available");

	*found = 0;

	wifi_scan_config_t scan_cfg = {
		.ssid        = NULL,
		.bssid       = NULL,
		.channel     = 0,        /* all channels */
		.show_hidden = false,
		.scan_type   = WIFI_SCAN_TYPE_ACTIVE,
	};

	ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan failed");

	uint16_t ap_num = 0;
	ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_num), TAG, "scan count failed");
	if (ap_num == 0) {
		return ESP_OK;
	}

	uint16_t to_fetch = (ap_num > BSP_WIFI_SCAN_MAX) ? BSP_WIFI_SCAN_MAX : ap_num;
	wifi_ap_record_t *records = calloc(to_fetch, sizeof(wifi_ap_record_t));
	if (!records) {
		esp_wifi_clear_ap_list();
		return ESP_ERR_NO_MEM;
	}

	esp_err_t err = esp_wifi_scan_get_ap_records(&to_fetch, records);
	if (err != ESP_OK) {
		free(records);
		return err;
	}

	size_t written = 0;
	for (uint16_t i = 0; i < to_fetch && (!out || written < max); i++) {
		if (out) {
			bsp_wifi_ap_t *dst = &out[written];
			memset(dst, 0, sizeof(*dst));
			strncpy(dst->ssid, (const char *)records[i].ssid, sizeof(dst->ssid) - 1);
			memcpy(dst->bssid, records[i].bssid, sizeof(dst->bssid));
			dst->rssi     = records[i].rssi;
			dst->channel  = records[i].primary;
			dst->authmode = records[i].authmode;
		}
		written++;
	}

	free(records);
	*found = out ? written : to_fetch;
	return ESP_OK;
}

esp_err_t bsp_wifi_scan_print(void)
{
	bsp_wifi_ap_t aps[BSP_WIFI_SCAN_MAX];
	size_t found = 0;

	ESP_LOGI(TAG, "Scanning for access points...");
	ESP_RETURN_ON_ERROR(bsp_wifi_scan(aps, BSP_WIFI_SCAN_MAX, &found), TAG, "scan failed");

	if (found == 0) {
		ESP_LOGW(TAG, "No access points found");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Found %u access point(s):", (unsigned)found);
	printf("  %-32s %-18s %4s %3s  %s\n", "SSID", "BSSID", "RSSI", "CH", "AUTH");
	for (size_t i = 0; i < found; i++) {
		printf("  %-32s %02x:%02x:%02x:%02x:%02x:%02x %4d %3u  %s\n",
		       aps[i].ssid[0] ? aps[i].ssid : "<hidden>",
		       aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
		       aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
		       aps[i].rssi, aps[i].channel,
		       bsp_wifi_authmode_str(aps[i].authmode));
	}
	return ESP_OK;
}

/* ===========================================================================
 * Association
 * ========================================================================= */

esp_err_t bsp_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
	ESP_RETURN_ON_FALSE(ssid, ESP_ERR_INVALID_ARG, TAG, "ssid is NULL");
	ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "wifi not available");

	wifi_config_t wifi_cfg = { 0 };
	strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
	if (password) {
		strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
	}
	/* Allow open networks too: leaving the threshold at WPA2 would silently
	 * refuse to associate with them. */
	wifi_cfg.sta.threshold.authmode = password && password[0]
	                                  ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

	ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");

	xEventGroupClearBits(s_wifi_events, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT);
	s_connect_requested = true;

	esp_err_t err = esp_wifi_connect();
	if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
		s_connect_requested = false;
		ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
	EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
	                                       BSP_WIFI_CONNECTED_BIT,
	                                       pdFALSE, pdFALSE,
	                                       pdMS_TO_TICKS(timeout_ms ? timeout_ms : 20000));

	if (!(bits & BSP_WIFI_CONNECTED_BIT)) {
		ESP_LOGE(TAG, "Timed out joining \"%s\"", ssid);
		return ESP_ERR_TIMEOUT;
	}

	ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
	return ESP_OK;
}

esp_err_t bsp_wifi_disconnect(void)
{
	if (!s_wifi_started) {
		return ESP_OK;
	}
	s_connect_requested = false;
	xEventGroupClearBits(s_wifi_events, BSP_WIFI_CONNECTED_BIT);
	return esp_wifi_disconnect();
}

bool bsp_wifi_is_connected(void)
{
	if (!s_wifi_started || !s_wifi_events) {
		return false;
	}
	return (xEventGroupGetBits(s_wifi_events) & BSP_WIFI_CONNECTED_BIT) != 0;
}

esp_err_t bsp_wifi_get_ip_str(char *out, size_t len)
{
	ESP_RETURN_ON_FALSE(out && len >= 16, ESP_ERR_INVALID_ARG, TAG, "buffer too small");

	/* Being offline is a normal state that callers poll, so do not log here. */
	if (!bsp_wifi_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	esp_netif_ip_info_t ip_info;
	ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_sta_netif, &ip_info), TAG, "no ip info");

	snprintf(out, len, IPSTR, IP2STR(&ip_info.ip));
	return ESP_OK;
}
