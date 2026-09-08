/*
 * Bluetooth LE support for the epdInky ESP32-P4/C6 board.
 *
 * The ESP32-P4 has no radio at all, so there is no local BLE controller. The
 * controller lives on the ESP32-C6 and is reached over the same esp-hosted SDIO
 * link as Wi-Fi, using HCI over SDIO. The P4 therefore runs only the NimBLE
 * *host* stack (CONFIG_BT_CONTROLLER_DISABLED), and esp-hosted transports HCI
 * packets to the C6 through a virtual HCI interface.
 *
 * Practical consequence: bsp_wifi_init() and bsp_ble_init() share one physical
 * link, and the C6 reports "BLE only" - classic Bluetooth is not available.
 */

#include <string.h>

#include "sdkconfig.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include "esp_hosted.h"
#include "esp_hosted_misc.h"

#include "bsp/epdinky_p4_board.h"

static const char *TAG = "epdinky_ble";

#define BSP_BLE_SYNCED_BIT    BIT0
#define BSP_BLE_SCAN_DONE_BIT BIT1

static bool               s_ble_started;
static EventGroupHandle_t s_ble_events;
static uint8_t            s_own_addr_type;

/* Results are filled in from the NimBLE host task, so they need a lock. */
static SemaphoreHandle_t  s_scan_lock;
static bsp_ble_device_t  *s_scan_out;
static size_t             s_scan_max;
static size_t             s_scan_count;

/* ===========================================================================
 * Host stack plumbing
 * ========================================================================= */

static void ble_on_sync(void)
{
	/* Controller is up; work out which address we should advertise/scan with. */
	int rc = ble_hs_util_ensure_addr(0);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
		return;
	}
	rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
		return;
	}
	xEventGroupSetBits(s_ble_events, BSP_BLE_SYNCED_BIT);
}

static void ble_on_reset(int reason)
{
	ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
	xEventGroupClearBits(s_ble_events, BSP_BLE_SYNCED_BIT);
}

static void ble_host_task(void *param)
{
	nimble_port_run();               /* returns only after nimble_port_stop() */
	nimble_port_freertos_deinit();
}

/* ===========================================================================
 * Scanning
 * ========================================================================= */

/* Advertisements repeat, so keep one entry per device and refresh its RSSI. */
static bsp_ble_device_t *scan_find_or_add(const ble_addr_t *addr)
{
	for (size_t i = 0; i < s_scan_count; i++) {
		if (s_scan_out[i].addr_type == addr->type &&
		    memcmp(s_scan_out[i].addr, addr->val, 6) == 0) {
			return &s_scan_out[i];
		}
	}
	if (s_scan_count >= s_scan_max) {
		return NULL;
	}
	bsp_ble_device_t *dev = &s_scan_out[s_scan_count++];
	memset(dev, 0, sizeof(*dev));
	memcpy(dev->addr, addr->val, 6);
	dev->addr_type = addr->type;
	return dev;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
	if (event->type == BLE_GAP_EVENT_DISC) {
		xSemaphoreTake(s_scan_lock, portMAX_DELAY);

		bsp_ble_device_t *dev = scan_find_or_add(&event->disc.addr);
		if (dev) {
			dev->rssi = event->disc.rssi;

			/* A device's name often arrives in a later scan response than the
			 * initial advertisement, so only overwrite when we actually got one. */
			struct ble_hs_adv_fields fields;
			if (ble_hs_adv_parse_fields(&fields, event->disc.data,
			                            event->disc.length_data) == 0) {
				if (fields.name != NULL && fields.name_len > 0) {
					size_t n = fields.name_len;
					if (n > sizeof(dev->name) - 1) {
						n = sizeof(dev->name) - 1;
					}
					memcpy(dev->name, fields.name, n);
					dev->name[n] = '\0';
				}
			}
		}

		xSemaphoreGive(s_scan_lock);
		return 0;
	}

	if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
		xEventGroupSetBits(s_ble_events, BSP_BLE_SCAN_DONE_BIT);
	}
	return 0;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t bsp_ble_init(void)
{
	if (s_ble_started) {
		return ESP_OK;
	}

	if (!s_ble_events) {
		s_ble_events = xEventGroupCreate();
		ESP_RETURN_ON_FALSE(s_ble_events, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
	}
	if (!s_scan_lock) {
		s_scan_lock = xSemaphoreCreateMutex();
		ESP_RETURN_ON_FALSE(s_scan_lock, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
	}

	ESP_LOGI(TAG, "Starting NimBLE host (controller on the ESP32-C6 via esp-hosted)");

	/*
	 * Since esp-hosted v2.5.2 the co-processor's BT controller is DISABLED at
	 * boot (so its MAC can be set first), even though the slave advertises BLE
	 * in its capabilities. The host must switch it on explicitly, otherwise
	 * every HCI command times out with BLE_HS_ETIMEOUT_HCI and the NimBLE host
	 * never syncs.
	 */
	if (esp_hosted_connect_to_slave() != 0) {
		ESP_LOGE(TAG, "esp-hosted transport is not available");
		return ESP_ERR_INVALID_STATE;
	}

	esp_err_t err = esp_hosted_bt_controller_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Remote BT controller init failed: %s", esp_err_to_name(err));
		return err;
	}
	err = esp_hosted_bt_controller_enable();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Remote BT controller enable failed: %s", esp_err_to_name(err));
		return err;
	}
	ESP_LOGI(TAG, "Remote BT controller enabled on the ESP32-C6");

	err = nimble_port_init();
	ESP_RETURN_ON_ERROR(err, TAG,
	                    "nimble_port_init failed - is the esp-hosted transport up?");

	ble_hs_cfg.sync_cb  = ble_on_sync;
	ble_hs_cfg.reset_cb = ble_on_reset;

	nimble_port_freertos_init(ble_host_task);

	/* The host is only usable once it has synced with the remote controller. */
	EventBits_t bits = xEventGroupWaitBits(s_ble_events, BSP_BLE_SYNCED_BIT,
	                                       pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
	if (!(bits & BSP_BLE_SYNCED_BIT)) {
		ESP_LOGE(TAG, "Timed out waiting for the BLE controller to sync");
		nimble_port_stop();
		nimble_port_deinit();
		return ESP_ERR_TIMEOUT;
	}

	s_ble_started = true;

	uint8_t addr[6] = {0};
	if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0) {
		/* NimBLE stores addresses little-endian; print MSB first. */
		ESP_LOGI(TAG, "BLE ready, address %02x:%02x:%02x:%02x:%02x:%02x (type %u)",
		         addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], s_own_addr_type);
	}
	return ESP_OK;
}

esp_err_t bsp_ble_deinit(void)
{
	if (!s_ble_started) {
		return ESP_OK;
	}
	ble_gap_disc_cancel();

	int rc = nimble_port_stop();
	if (rc == 0) {
		nimble_port_deinit();
	}
	s_ble_started = false;
	return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_ble_scan(bsp_ble_device_t *out, size_t max, size_t *found,
                       uint32_t duration_ms)
{
	ESP_RETURN_ON_FALSE(out && max > 0, ESP_ERR_INVALID_ARG, TAG, "bad output buffer");
	ESP_RETURN_ON_FALSE(found, ESP_ERR_INVALID_ARG, TAG, "found is NULL");
	ESP_RETURN_ON_ERROR(bsp_ble_init(), TAG, "ble not available");

	*found = 0;
	if (duration_ms == 0) {
		duration_ms = 5000;
	}

	xSemaphoreTake(s_scan_lock, portMAX_DELAY);
	s_scan_out   = out;
	s_scan_max   = max;
	s_scan_count = 0;
	xSemaphoreGive(s_scan_lock);

	struct ble_gap_disc_params params = {
		.itvl          = 0,      /* 0 = let the stack pick a sensible default */
		.window        = 0,
		.filter_policy = 0,
		.limited       = 0,
		.passive       = 0,      /* active: also request scan responses (names) */
		.filter_duplicates = 0,  /* we de-duplicate ourselves, keeping best RSSI */
	};

	xEventGroupClearBits(s_ble_events, BSP_BLE_SCAN_DONE_BIT);

	int rc = ble_gap_disc(s_own_addr_type, duration_ms, &params, ble_gap_event_cb, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
		return ESP_FAIL;
	}

	/* Wait a little beyond the scan window so the completion event can land. */
	EventBits_t bits = xEventGroupWaitBits(s_ble_events, BSP_BLE_SCAN_DONE_BIT,
	                                       pdFALSE, pdFALSE,
	                                       pdMS_TO_TICKS(duration_ms + 2000));
	if (!(bits & BSP_BLE_SCAN_DONE_BIT)) {
		ESP_LOGW(TAG, "Scan did not report completion; cancelling");
		ble_gap_disc_cancel();
	}

	xSemaphoreTake(s_scan_lock, portMAX_DELAY);
	*found     = s_scan_count;
	s_scan_out = NULL;   /* stop the callback writing into the caller's buffer */
	s_scan_max = 0;
	xSemaphoreGive(s_scan_lock);

	return ESP_OK;
}

esp_err_t bsp_ble_scan_print(uint32_t duration_ms)
{
	static bsp_ble_device_t devices[BSP_BLE_SCAN_MAX];
	size_t found = 0;

	ESP_LOGI(TAG, "Scanning for BLE devices for %u ms...",
	         (unsigned)(duration_ms ? duration_ms : 5000));
	ESP_RETURN_ON_ERROR(bsp_ble_scan(devices, BSP_BLE_SCAN_MAX, &found, duration_ms),
	                    TAG, "scan failed");

	if (found == 0) {
		ESP_LOGW(TAG, "No BLE devices found");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Found %u BLE device(s):", (unsigned)found);
	printf("  %-18s %-6s %4s  %s\n", "ADDRESS", "TYPE", "RSSI", "NAME");
	for (size_t i = 0; i < found; i++) {
		const uint8_t *a = devices[i].addr;
		printf("  %02x:%02x:%02x:%02x:%02x:%02x %-6s %4d  %s\n",
		       a[5], a[4], a[3], a[2], a[1], a[0],
		       devices[i].addr_type == BLE_ADDR_PUBLIC ? "public" : "random",
		       devices[i].rssi,
		       devices[i].name[0] ? devices[i].name : "<no name>");
	}
	return ESP_OK;
}

bool bsp_ble_is_ready(void)
{
	return s_ble_started;
}

#else /* !CONFIG_BT_NIMBLE_ENABLED */

#include "esp_err.h"
#include "esp_log.h"
#include "bsp/epdinky_p4_board.h"

static const char *TAG = "epdinky_ble";

static esp_err_t ble_not_enabled(void)
{
	ESP_LOGE(TAG, "BLE support is not compiled in "
	              "(enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED)");
	return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_ble_init(void) { return ble_not_enabled(); }
esp_err_t bsp_ble_deinit(void) { return ESP_OK; }
esp_err_t bsp_ble_scan(bsp_ble_device_t *out, size_t max, size_t *found, uint32_t duration_ms)
{
	(void)out; (void)max; (void)duration_ms;
	if (found) { *found = 0; }
	return ble_not_enabled();
}
esp_err_t bsp_ble_scan_print(uint32_t duration_ms) { (void)duration_ms; return ble_not_enabled(); }
bool bsp_ble_is_ready(void) { return false; }

#endif /* CONFIG_BT_NIMBLE_ENABLED */
