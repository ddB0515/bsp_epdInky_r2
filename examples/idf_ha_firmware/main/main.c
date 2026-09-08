/*
 * Home Assistant TFT dashboard - the always-on cycle.
 *
 * Unlike examples/idf_epd_ha_firmware, there is no wake/sleep cycle here:
 * board bring-up and provisioning happen once, then the dashboard just runs
 * forever, driven by ha_lvgl.c's LVGL task and ha_ws.c's WebSocket task. This
 * file's own job after setup is just polling the physical button for the
 * reconfigure/factory-reset gestures - see ha_button.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/epdinky_p4_board.h"

#include "ha_button.h"
#include "ha_config.h"
#include "ha_dashboard.h"
#include "ha_dashboard_config.h"
#include "ha_lvgl.h"
#include "ha_panel.h"
#include "ha_persist.h"
#include "ha_setup_screen.h"
#include "ha_ws.h"
#include "portal/ha_portal.h"

static const char *TAG = "ha_main";

static bsp_epdinky_handles_t s_board;

/* ===========================================================================
 * Provisioning gates - same shape as examples/idf_epd_ha_firmware/main.c's
 * needs_ha_config()/force_ha_config(), NVS keys swapped for this firmware's
 * (see ha_config.h).
 * ========================================================================= */

static bool needs_wifi(void)
{
    return !ha_persist_exists(HA_NVS_WIFI_SSID);
}

static bool needs_ha_config(void)
{
    /* Only the host is strictly required - see
     * portal/ha_portal.c:handle_config_connect()'s own validation. The token
     * may legitimately be blank for a Home Assistant setup that doesn't
     * need one in front of its WebSocket API. */
    char host[HA_WS_HOST_MAX] = { 0 };
    ha_persist_get_str(HA_NVS_WS_HOST, host, sizeof(host));
    return host[0] == '\0';
}

static bool force_ha_config(void)
{
    return ha_persist_get_u32(HA_NVS_FORCE_CFG, 0) != 0;
}

/* ===========================================================================
 * Button gesture handlers
 * ========================================================================= */

static void handle_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset: erasing Wi-Fi and Home Assistant settings");
    ha_persist_erase(HA_NVS_WIFI_SSID);
    ha_persist_erase(HA_NVS_WIFI_PASS);
    ha_persist_erase(HA_NVS_WS_HOST);
    ha_persist_erase(HA_NVS_WS_PORT);
    ha_persist_erase(HA_NVS_WS_TOKEN);
    ha_persist_erase(HA_NVS_FORCE_CFG);
    esp_restart();
}

static void handle_reconfigure(void)
{
    ESP_LOGW(TAG, "reconfigure: dropping back into Home Assistant setup");
    ha_persist_set_u32(HA_NVS_FORCE_CFG, 1);
    esp_restart();
}

/*
 * Polls the button forever for the reconfigure/factory-reset gestures - the
 * one thing left for app_main()'s own task to do once the dashboard (or a
 * provisioning screen) is up and running on the LVGL/WebSocket tasks.
 *
 * Also used while a provisioning screen is showing: the portal itself
 * reboots on a successful submission from inside its own httpd task (see
 * portal/ha_portal.c), so there is nothing else for this loop to wait for
 * besides that reboot - it is not "the code path taken only after setup".
 */
static void idle_forever(void)
{
    while (true) {
        if (!bsp_button_is_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        switch (ha_button_wait_gesture()) {
        case HA_BUTTON_SHORT:
            ha_dashboard_wake_backlight();
            break;
        case HA_BUTTON_RECONFIGURE:
            handle_reconfigure();
            break;
        case HA_BUTTON_FACTORY_RESET:
            handle_factory_reset();
            break;
        case HA_BUTTON_NONE:
        default:
            break;
        }
    }
}

/* ===========================================================================
 * Wi-Fi
 * ========================================================================= */

/*
 * Retries in place rather than rebooting on failure: this is an always-on
 * mains-powered wall panel, not a battery device with a cycle to abandon - a
 * router blip shouldn't take the screen dark and restart it.
 */
static void connect_wifi_or_wait(void)
{
    char ssid[HA_WIFI_SSID_MAX] = { 0 };
    char pass[HA_WIFI_PASS_MAX] = { 0 };
    ha_persist_get_str(HA_NVS_WIFI_SSID, ssid, sizeof(ssid));
    ha_persist_get_str(HA_NVS_WIFI_PASS, pass, sizeof(pass));

    ESP_ERROR_CHECK(bsp_wifi_init());

    while (true) {
        if (bsp_wifi_is_connected()) {
            return;
        }
        esp_err_t err = bsp_wifi_connect(ssid, pass, HA_WIFI_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK) {
            return;
        }
        ESP_LOGW(TAG, "Wi-Fi connect failed (%s); retrying", esp_err_to_name(err));
        ha_setup_screen_show_message("Wi-Fi trouble",
                                     "Retrying the connection - hold the button "
                                     "5-20s to reconfigure Home Assistant, or "
                                     "over 20s for a full factory reset.");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ===========================================================================
 * app_main
 * ========================================================================= */

void app_main(void)
{
    bsp_epdinky_config_t cfg = bsp_epdinky_default_config();
    /* This board has no e-paper panel or battery fitted - it's a TFT+touch
     * build, mains-powered. See the project plan for why these three are the
     * only bring-up flags touched relative to the BSP's own defaults. */
    cfg.enable.use_tps65185 = false; /* e-paper PMIC, not present */
    cfg.enable.use_stc3115  = false; /* fuel gauge, no battery to gauge */
    cfg.enable.use_epd_gpio = false; /* no EPD parallel bus wired */

    ESP_ERROR_CHECK(bsp_epdinky_init_with_config(&cfg, &s_board));

    ESP_ERROR_CHECK(ha_panel_init());
    ESP_ERROR_CHECK(ha_lvgl_init());
    /*
     * LVGL comes up before any provisioning below - on purpose. This panel
     * is the only way anyone ever learns the SoftAP SSID, its QR code or the
     * Stage 2 portal URL: there's no serial console to fall back to once
     * this thing is on a wall.
     */

    ESP_ERROR_CHECK(ha_persist_init());

    if (needs_wifi()) {
        ESP_ERROR_CHECK(ha_portal_run_wifi());
        idle_forever(); /* the portal reboots on submit */
    }

    connect_wifi_or_wait();

    if (needs_ha_config() || force_ha_config()) {
        ESP_ERROR_CHECK(ha_portal_run_config());
        idle_forever(); /* the portal reboots on submit */
    }

    char     host[HA_WS_HOST_MAX]   = { 0 };
    char     token[HA_WS_TOKEN_MAX] = { 0 };
    uint32_t port = ha_persist_get_u32(HA_NVS_WS_PORT, HA_WS_PORT_DEFAULT);
    ha_persist_get_str(HA_NVS_WS_HOST, host, sizeof(host));
    ha_persist_get_str(HA_NVS_WS_TOKEN, token, sizeof(token));

    const char *entity_ids[HA_DASHBOARD_TILE_COUNT];
    for (size_t i = 0; i < HA_DASHBOARD_TILE_COUNT; i++) {
        entity_ids[i] = HA_DASHBOARD_TILES[i].entity_id;
    }

    ha_ws_config_t ws_cfg = {
        .host  = host,
        .port  = (uint16_t)port,
        .token = token,
    };
    /* ha_ws_init() only copies config and allocates local state - no
     * networking happens until ha_ws_start() below, so ha_dashboard_init()
     * can safely register its callbacks and do its initial
     * ha_ws_get_state() pass in between, with nothing racing it. */
    ESP_ERROR_CHECK(ha_ws_init(&ws_cfg, entity_ids, HA_DASHBOARD_TILE_COUNT));
    ESP_ERROR_CHECK(ha_dashboard_init());
    ESP_ERROR_CHECK(ha_ws_start());

    ESP_LOGI(TAG, "epdInky HA dashboard firmware %s ready", HA_FW_VERSION_STRING);
    idle_forever();
}
