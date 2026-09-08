/*
 * Home Assistant e-paper firmware for the epdInky ESP32-P4/C6 board.
 *
 * WHAT THIS BUILD DOES
 *
 * Every wake:
 *   1. brings the board up - PMIC, panel, framebuffer, RTC, fuel gauge,
 *      button - then:
 *   2. joins Wi-Fi through the BSP (esp-hosted -> the on-board ESP32-C6), or
 *      falls back to a captive-portal SoftAP if no network is provisioned
 *      yet (stage 1 - Wi-Fi only);
 *   2b. once connected, if MQTT/the dashboard URL are not yet configured,
 *       shows "browse to http://<ip>/" and runs a plain config server on
 *       that address until they are (stage 2), then reboots;
 *   3. sets the clock: RV-3028 at boot, SNTP once a day, written back;
 *   4. connects to the MQTT broker from NVS, publishes Home Assistant MQTT
 *      discovery configs and current state (battery voltage/percent/
 *      charging, Wi-Fi RSSI, last refresh time), briefly listens for a
 *      "refresh now" button press from Home Assistant, then disconnects;
 *   5. HTTP GETs the dashboard image from the configured URL (with a Bearer
 *      token if one was provided) and shows it on the panel;
 *   6. sleeps - idle or deep, per Kconfig - and goes round again. In idle
 *      builds, wait_for_next_cycle() also keeps a background MQTT listener
 *      up for the whole wait (not just step 4's brief window), so a
 *      "refresh now" press ends the wait immediately - deep sleep can't do
 *      this, same reason the physical button can't wake it either.
 *
 * This firmware renders nothing itself. Step 5 fetches whatever PNG a
 * dashboard-render service (out of scope here - see the README) returns; the
 * only thing rendered on-device is the plain-text status screens for the
 * error arms below.
 *
 * There is no server protocol to be compatible with, unlike the
 * trmnl-firmware example this one borrows its provisioning/display/sleep
 * shape from: no registration step, no vendor-specific retry ladder. A
 * failure in any one arm just shortens the next sleep (HA_RETRY_INTERVAL_S)
 * instead of waiting out the full refresh interval, and shows a plain status
 * screen on its first occurrence so a real fault is visible on the glass,
 * not just in the log.
 *
 * Wi-Fi, MQTT, the dashboard URL/token and the refresh interval all live
 * only in NVS - there is no compiled-in fallback for any of them. A device
 * with nothing in NVS yet, or one whose button was just held past 20 s (the
 * "factory reset" gesture), drops into ha_portal_run_wifi() (main/portal/):
 * a SoftAP + captive portal that saves the Wi-Fi network and reboots. A
 * device with Wi-Fi but no MQTT/dashboard configuration yet - fresh out of
 * stage 1, or one whose button was just held 5-20 s (the "reconfigure"
 * gesture) - drops into ha_portal_run_config() instead: a plain HTTP server
 * on its real station IP that collects everything else and reboots.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "bsp/epdinky_p4_board.h"

#include "ha_battery.h"
#include "ha_button.h"
#include "ha_config.h"
#include "ha_display.h"
#include "ha_http.h"
#include "ha_mqtt.h"
#include "ha_persist.h"
#include "ha_sleep.h"
#include "ha_time.h"
#include "portal/ha_portal.h"

static const char *TAG = "ha_epdinky";

static bsp_epdinky_handles_t s_board;
static bsp_epdinky_config_t  s_board_cfg;

/*
 * Wall clock of the last cycle that actually got a fresh image onto the
 * panel - the MQTT "last refresh" sensor. RTC_DATA_ATTR so it survives a
 * deep sleep (where every cycle is a fresh boot) but not a power cycle,
 * which is fine: there is nothing meaningful to report before the first
 * successful cycle after power-on either way.
 */
RTC_DATA_ATTR static uint32_t s_last_refresh_unix;

/*
 * Whether the next image should be preceded by full black/white clean
 * cycles. True on cold boot and after any status screen - large text ghosts
 * badly behind a photograph - false between routine dashboard updates.
 */
RTC_DATA_ATTR static bool s_needs_clean = true;

/* ===========================================================================
 * NVS-backed configuration, read fresh each cycle
 * ========================================================================= */

static const char *wifi_ssid(void)
{
    static char ssid[HA_WIFI_SSID_MAX];
    ha_persist_get_str(HA_NVS_WIFI_SSID, ssid, sizeof(ssid));
    return ssid;
}

static const char *wifi_password(void)
{
    static char password[HA_WIFI_PASS_MAX];
    ha_persist_get_str(HA_NVS_WIFI_PASS, password, sizeof(password));
    return password;
}

static uint32_t refresh_interval_s(void)
{
    uint32_t s = ha_persist_get_u32(HA_NVS_REFRESH_S, CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S);
    if (s < HA_REFRESH_S_MIN || s > HA_REFRESH_S_MAX) {
        s = CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S;
    }
    return s;
}

static int current_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

/* ===========================================================================
 * Button-triggered resets
 * ========================================================================= */

/** @return true if either the MQTT broker or the dashboard image URL is not
 *  yet configured - the gate for dropping into the stage-2 config server. */
static bool needs_ha_config(void)
{
    char host[HA_MQTT_HOST_MAX];
    char url[HA_IMAGE_URL_MAX];
    ha_persist_get_str(HA_NVS_MQTT_HOST, host, sizeof(host));
    ha_persist_get_str(HA_NVS_IMAGE_URL, url, sizeof(url));
    return host[0] == '\0' || url[0] == '\0';
}

/** @return true if the "reconfigure" gesture asked for the stage-2 config
 *  server on the next cycle, regardless of whether existing settings are
 *  already complete. Cleared once the form is actually submitted
 *  (ha_portal_run_config()'s /connect handler). */
static bool force_ha_config(void)
{
    return ha_persist_get_u32(HA_NVS_FORCE_CFG, 0) != 0;
}

/** Button held 5-20 s: leave the stored MQTT/dashboard settings alone and
 *  just flag that the stage-2 config server should run next cycle, then
 *  reboot. Because nothing is erased, that server can pre-fill its form
 *  with what's already stored - the whole point of this gesture over the
 *  factory-reset one below. */
static void handle_reconfigure_ha(void)
{
    ESP_LOGW(TAG, "button held 5-20 s: reopening MQTT/dashboard setup, rebooting");
    ha_display_message("Reconfigure", "Opening MQTT/dashboard setup", "rebooting...");

    ha_persist_set_u32(HA_NVS_FORCE_CFG, 1);

    esp_restart();
    /* Not reached. */
}

/** Button held past 20 s: clear everything, including Wi-Fi, and reboot into
 *  the stage-1 captive portal. The one "start completely over" gesture. */
static void handle_factory_reset(void)
{
    ESP_LOGW(TAG, "button held >20 s: erasing all configuration, rebooting into setup");
    ha_display_message("Factory reset", "Erasing Wi-Fi/MQTT/dashboard setup", "rebooting...");

    ha_persist_erase(HA_NVS_WIFI_SSID);
    ha_persist_erase(HA_NVS_WIFI_PASS);
    ha_persist_erase(HA_NVS_MQTT_HOST);
    ha_persist_erase(HA_NVS_MQTT_PORT);
    ha_persist_erase(HA_NVS_MQTT_USER);
    ha_persist_erase(HA_NVS_MQTT_PASS);
    ha_persist_erase(HA_NVS_IMAGE_URL);
    ha_persist_erase(HA_NVS_HA_TOKEN);
    ha_persist_erase(HA_NVS_REFRESH_S);
    ha_persist_erase(HA_NVS_FORCE_CFG);

    esp_restart();
    /* Not reached. */
}

/* ===========================================================================
 * MQTT: telemetry, discovery, refresh-button
 * ========================================================================= */

/** @return true if a "refresh now" button press was seen from Home
 *  Assistant during the brief listen window - not otherwise acted on
 *  differently here, since every cycle already fetches a fresh image, but
 *  logged so the button's effect is visible even on a build with no local
 *  button (deep-sleep mode, or no press this cycle). */
static bool run_mqtt_phase(void)
{
    char host[HA_MQTT_HOST_MAX];
    char user[HA_MQTT_USER_MAX];
    char pass[HA_MQTT_PASS_MAX];
    char device_id[32];

    ha_persist_get_str(HA_NVS_MQTT_HOST, host, sizeof(host));
    if (host[0] == '\0') {
        ESP_LOGW(TAG, "no MQTT broker configured; skipping telemetry this cycle");
        return false;
    }
    ha_persist_get_str(HA_NVS_MQTT_USER, user, sizeof(user));
    ha_persist_get_str(HA_NVS_MQTT_PASS, pass, sizeof(pass));

    if (ha_mqtt_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        ESP_LOGW(TAG, "could not derive an MQTT device id (no STA MAC yet?); skipping");
        return false;
    }

    ha_battery_status_t battery;
    ha_battery_read(&battery);
    if (battery.present) {
        ESP_LOGI(TAG, "Battery: %.2f V, %.0f%%%s", battery.voltage_v, battery.percent,
                 battery.charging ? " (charging)" : "");
    } else {
        ESP_LOGI(TAG, "no battery detected");
    }

    char last_refresh[32];
    ha_time_format_iso8601((time_t)s_last_refresh_unix, last_refresh, sizeof(last_refresh));

    /*
     * Generously above the refresh interval, not equal to it: a single
     * retried or delayed cycle (HA_RETRY_INTERVAL_S) shouldn't flap every
     * entity to "unavailable" and back. Two missed refresh intervals in a
     * row genuinely is worth flagging as stale, though.
     */
    const uint32_t state_max_age_s = 2 * refresh_interval_s() + HA_RETRY_INTERVAL_S;

    const ha_mqtt_config_t cfg = {
        .host            = host,
        .port            = (uint16_t)ha_persist_get_u32(HA_NVS_MQTT_PORT, CONFIG_HA_MQTT_DEFAULT_PORT),
        .username        = user,
        .password        = pass,
        .device_id       = device_id,
        .state_max_age_s = state_max_age_s,
    };
    const ha_mqtt_state_t state = {
        .battery_voltage_v    = battery.voltage_v,
        .battery_present      = battery.present,
        .battery_percent      = battery.percent,
        .battery_charging     = battery.charging,
        .wifi_rssi_dbm        = current_rssi(),
        .last_refresh_iso8601 = last_refresh,
    };

    bool refresh_requested = false;
    esp_err_t err = ha_mqtt_run_cycle(&cfg, &state, CONFIG_HA_MQTT_COMMAND_WINDOW_S,
                                     &refresh_requested);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT failed: %s (%s:%" PRIu32 ")", esp_err_to_name(err),
                 host, ha_persist_get_u32(HA_NVS_MQTT_PORT, CONFIG_HA_MQTT_DEFAULT_PORT));
        /*
         * Shown even though the image fetch that follows will likely
         * overwrite it a moment later - the point is that a broker outage
         * that persists (rather than being one flaky cycle) still leaves a
         * visible trail on the glass, not just in a log nobody is watching a
         * battery-powered display's serial port for. Also marks the next
         * flush for a clean pass, since a status screen ghosts badly behind
         * whatever image follows it.
         */
        ha_display_message("MQTT failed", esp_err_to_name(err), "still fetching the dashboard...");
        s_needs_clean = true;
    }

    if (refresh_requested) {
        ESP_LOGI(TAG, "Home Assistant requested a refresh (already fetching one this cycle)");
    }
    return refresh_requested;
}

/* ===========================================================================
 * Dashboard image fetch and display
 * ========================================================================= */

static esp_err_t run_image_phase(void)
{
    char url[HA_IMAGE_URL_MAX];
    char token[HA_TOKEN_MAX];
    ha_persist_get_str(HA_NVS_IMAGE_URL, url, sizeof(url));
    ha_persist_get_str(HA_NVS_HA_TOKEN, token, sizeof(token));

    if (url[0] == '\0') {
        ESP_LOGW(TAG, "no dashboard image URL configured");
        ha_display_message("Not configured", "No dashboard image URL set",
                           "use the captive portal to add one");
        s_needs_clean = true;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "--- fetching the dashboard image ---");

    ha_http_response_t resp;
    esp_err_t err = ha_http_get(url, token, HA_IMAGE_MAX_BYTES, HA_HTTP_TIMEOUT_MS, &resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image fetch failed: %s", esp_err_to_name(err));
        ha_display_message("Image fetch failed", esp_err_to_name(err), url);
        s_needs_clean = true;
        ha_http_response_free(&resp);
        return err;
    }

    ESP_LOGI(TAG, "HTTP %d, Content-Type %s, %u bytes%s",
             resp.status, resp.content_type[0] ? resp.content_type : "(none)",
             (unsigned)resp.body_len, resp.truncated ? " (TRUNCATED at the cap)" : "");

    if (resp.status < 200 || resp.status >= 300 || resp.body_len == 0) {
        char detail[48];
        snprintf(detail, sizeof(detail), "HTTP %d, %u bytes", resp.status, (unsigned)resp.body_len);
        ha_display_message("Image fetch failed", detail, url);
        s_needs_clean = true;
        ha_http_response_free(&resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp.truncated) {
        ha_display_message("Image fetch failed", "response exceeded the size cap",
                           "raise HA_IMAGE_MAX_BYTES if this is a real image");
        s_needs_clean = true;
        ha_http_response_free(&resp);
        return ESP_ERR_INVALID_SIZE;
    }

    err = ha_display_show_image(resp.body, resp.body_len, s_needs_clean);
    ha_http_response_free(&resp);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decode/display failed: %s", esp_err_to_name(err));
        ha_display_message("Image fetch failed", "could not decode the response",
                           esp_err_to_name(err));
        s_needs_clean = true;
        return err;
    }

    s_needs_clean = false;
    ESP_LOGI(TAG, "dashboard is on the panel");
    if (ha_time_is_valid()) {
        s_last_refresh_unix = (uint32_t)time(NULL);
    }
    return ESP_OK;
}

/* ===========================================================================
 * One pass
 * ========================================================================= */

static uint32_t run_cycle(void)
{
    char now[24];
    ha_time_str(now, sizeof(now));
    ESP_LOGI(TAG, "=== cycle: %s UTC, woken by %s ===", now, ha_sleep_wake_reason());

    /* ---- button gesture, if idling ----------------------------------------- */

    switch (ha_sleep_button_gesture()) {
    case HA_BUTTON_FACTORY_RESET:
        handle_factory_reset();
        break; /* not reached */
    case HA_BUTTON_RECONFIGURE:
        handle_reconfigure_ha();
        break; /* not reached */
    default:
        break;
    }

    /* ---- Wi-Fi -------------------------------------------------------------
     *
     * The one truly gating failure: nothing past this point can do anything
     * useful without a network, so this is the only arm that skips the rest
     * of the cycle outright rather than best-effort continuing.
     */
    if (!bsp_wifi_is_connected()) {
        ESP_LOGI(TAG, "Connecting to \"%s\"...", wifi_ssid());

        const esp_err_t err = bsp_wifi_connect(wifi_ssid(), wifi_password(),
                                               HA_WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi failed: %s", esp_err_to_name(err));
            ha_display_message("Wi-Fi failed", wifi_ssid(), esp_err_to_name(err));
            s_needs_clean = true;
            return HA_RETRY_INTERVAL_S;
        }

        char ip[16] = "?";
        bsp_wifi_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "Connected: IP %s, RSSI %d dBm", ip, current_rssi());
    }

    /* ---- MQTT/dashboard config, if not set yet or explicitly requested -----
     *
     * Stage 2 of provisioning: a device that has only ever been through the
     * Wi-Fi-only captive portal has a real IP but nothing else it needs, so
     * needs_ha_config() sends it here. force_ha_config() gets it here too
     * even when everything is already set, for the button's "reconfigure"
     * gesture - the difference is that gesture never erased anything, so the
     * form ha_portal_run_config() serves can pre-fill from what's already in
     * NVS instead of asking for it all again. Does not return except on its
     * own startup failure - it reboots once settings are saved, or
     * deep-sleeps and retries after 15 minutes of inactivity.
     */
    if (needs_ha_config() || force_ha_config()) {
        const esp_err_t err = ha_portal_run_config(&s_board);
        ESP_LOGE(TAG, "config server failed to start: %s", esp_err_to_name(err));
        s_needs_clean = true;
        return HA_RETRY_INTERVAL_S;
    }

    /* ---- Clock -------------------------------------------------------------
     *
     * Gated, so this is a network round trip once a day rather than once a
     * cycle.
     */
    if (ha_time_sync_due()) {
        ha_time_sync_sntp(HA_SNTP_TIMEOUT_MS);
    }

    /* ---- MQTT: discovery, state, brief refresh-button listen -------------- */

    run_mqtt_phase();

    /* ---- Dashboard image ---------------------------------------------------
     *
     * Independent of the MQTT phase above: a broker that is unreachable
     * costs this cycle's telemetry, not the dashboard refresh, which is the
     * whole point of a battery-powered display.
     */
    const esp_err_t image_err = run_image_phase();

    return (image_err == ESP_OK) ? refresh_interval_s() : HA_RETRY_INTERVAL_S;
}

/* ===========================================================================
 * The wait, with a background MQTT listener in idle builds
 * ========================================================================= */

/* Set from the MQTT task's context by ha_mqtt_listen_start()'s handler, read
 * from the main task's context by ha_sleep_wait()'s poll loop. Never needs
 * RTC_DATA_ATTR: it's only ever used within one idle wait, and idle builds
 * never sleep the CPU between cycles in the first place. */
static volatile bool s_mqtt_refresh_requested;

/**
 * Wraps ha_sleep_wait() so a "Refresh Now" press in Home Assistant can end
 * an idle wait immediately - the same as a physical short button-press -
 * instead of only being caught in the few seconds run_mqtt_phase() spends
 * connected at the top of the next cycle. Deep-sleep builds skip the
 * listener entirely: the CPU is off for the whole wait, so there is nothing
 * that could keep a connection open to catch a press with.
 */
static void wait_for_next_cycle(uint32_t next_s)
{
    if (ha_sleep_is_deep()) {
        ha_sleep_wait(next_s, &s_board, NULL);
        return; /* not reached */
    }

    s_mqtt_refresh_requested = false;

    char host[HA_MQTT_HOST_MAX];
    char device_id[32];
    ha_persist_get_str(HA_NVS_MQTT_HOST, host, sizeof(host));
    const bool have_broker = host[0] != '\0' &&
                             ha_mqtt_device_id(device_id, sizeof(device_id)) == ESP_OK;

    if (have_broker) {
        char user[HA_MQTT_USER_MAX];
        char pass[HA_MQTT_PASS_MAX];
        ha_persist_get_str(HA_NVS_MQTT_USER, user, sizeof(user));
        ha_persist_get_str(HA_NVS_MQTT_PASS, pass, sizeof(pass));

        const ha_mqtt_config_t cfg = {
            .host      = host,
            .port      = (uint16_t)ha_persist_get_u32(HA_NVS_MQTT_PORT, CONFIG_HA_MQTT_DEFAULT_PORT),
            .username  = user,
            .password  = pass,
            .device_id = device_id,
        };
        if (ha_mqtt_listen_start(&cfg, &s_mqtt_refresh_requested) != ESP_OK) {
            ESP_LOGW(TAG, "could not start the idle-wait MQTT listener; a Home "
                     "Assistant refresh will only be caught next cycle");
        }
    }

    ha_sleep_wait(next_s, &s_board, &s_mqtt_refresh_requested);
    ha_mqtt_listen_stop();
}

/* ===========================================================================
 * Main
 * ========================================================================= */

void app_main(void)
{
    ESP_LOGI(TAG, "HA e-paper firmware %s, panel %dx%d",
             HA_FW_VERSION_STRING, HA_DISPLAY_WIDTH, HA_DISPLAY_HEIGHT);
    ESP_LOGI(TAG, "between cycles: %s", ha_sleep_is_deep()
             ? "deep sleep, timer wake only (the button does nothing)"
             : "idle, button polled");

    /* ---- Board -------------------------------------------------------------
     *
     * One board init, so the RTC handle the clock needs and the fuel gauge
     * handle the battery telemetry needs both come from the same place.
     * use_sdcard stays false: this firmware always fetches fresh each cycle
     * and has no offline image cache to need the card for.
     */
    s_board_cfg = bsp_epdinky_default_config();
    s_board_cfg.enable.use_tca6408  = true;
    s_board_cfg.enable.use_tps65185 = true;
    s_board_cfg.enable.use_rv3028   = true;
    s_board_cfg.enable.use_button   = true;
    s_board_cfg.enable.use_epd_gpio = false;
    s_board_cfg.enable.use_kxtj3    = false;
    s_board_cfg.enable.use_stc3115  = true;
    s_board_cfg.enable.use_sdcard   = false;

    esp_err_t err = bsp_epdinky_init_with_config(&s_board_cfg, &s_board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
        return;
    }

    ha_battery_init(s_board.stc3115);

    err = ha_display_init(s_board.tps65185);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s - continuing without a panel",
                 esp_err_to_name(err));
    }

    /* ---- Storage ------------------------------------------------------------ */

    ESP_ERROR_CHECK(ha_persist_init());

    /* ---- Provisioning --------------------------------------------------------
     *
     * Ahead of the clock: neither the clock nor Wi-Fi are up yet, so a
     * portal timeout has nothing to tear down before it sleeps. No stored
     * SSID - a fresh device, or one just reset via the button's
     * factory-reset gesture - means straight into the Wi-Fi-only portal,
     * with no compiled-in fallback. MQTT/dashboard configuration (stage 2)
     * is handled later, per-cycle, once Wi-Fi is actually connected - see
     * needs_ha_config() in run_cycle().
     */
    if (!ha_persist_exists(HA_NVS_WIFI_SSID)) {
        const esp_err_t portal_err = ha_portal_run_wifi(&s_board);
        if (portal_err != ESP_OK) {
            ESP_LOGE(TAG, "provisioning portal failed to start: %s",
                     esp_err_to_name(portal_err));
            /* Falls through: bsp_wifi_connect() below will fail on an empty
             * SSID and land on the existing retry interval. */
        }
    }

    /* Seed the clock from the RTC before the network exists. */
    ha_time_init(s_board.rv3028);

    /* ---- Wi-Fi ---------------------------------------------------------------
     *
     * Brings up NVS, the default event loop and the esp-hosted link to the
     * C6. Expect roughly 1.5 s here for the radio to reset and re-negotiate
     * SDIO.
     */
    ESP_ERROR_CHECK(bsp_wifi_init());

    /* ---- The cycle -------------------------------------------------------
     *
     * One iteration per boot in a deep-sleep build - ha_sleep_wait() does
     * not return there - and one per refresh interval when idling.
     */
    for (;;) {
        const uint32_t next_s = run_cycle();
        wait_for_next_cycle(next_s);
    }
}
