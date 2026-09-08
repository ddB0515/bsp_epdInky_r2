/*
 * Home Assistant MQTT integration for one wake cycle.
 *
 * Uses ESP-IDF's esp-mqtt client (fetched as the managed component
 * espressif/mqtt - see main/idf_component.yml; despite the name this is the
 * same esp-mqtt client IDF has shipped for years, just distributed via the
 * component registry rather than in-tree in this IDF release). No TRMNL-style
 * external protocol is involved: this talks plain MQTT to whatever broker the
 * captive portal was told about, and announces itself to Home Assistant via
 * the standard MQTT discovery convention
 * (homeassistant/<component>/<node_id>/<object_id>/config).
 *
 * One call, ha_mqtt_run_cycle(), does the whole of one wake's MQTT work:
 * connect, announce "online" on the availability topic (with an LWT in place
 * for "offline" if the connection ever drops uncleanly), publish retained
 * discovery configs and current state, listen briefly on the refresh-button
 * command topic, then disconnect. This firmware is not a long-lived MQTT
 * client - it is reachable for a few seconds once per cycle - but it does
 * *not* announce "offline" on every one of those disconnects: doing so made
 * every sensor show "Unavailable" in Home Assistant for all but a few
 * seconds out of every refresh interval, which defeats the point of
 * publishing telemetry at all. Instead each sensor/binary_sensor's discovery
 * config carries `expire_after` (see ha_mqtt_config_t::state_max_age_s), so
 * Home Assistant keeps showing the last real reading until it's actually
 * stale - the LWT remains the fallback for a genuine crash or network drop.
 *
 * Connects over plain TCP only (no MQTTS) in this example: the captive
 * portal collects a host and port, not a certificate, which is the right
 * scope for talking to a broker on the same local network as the rest of a
 * Home Assistant install. See the README if you need to reach a broker over
 * TLS.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Broker connection details, read out of NVS by the caller. */
typedef struct {
    const char *host;       /**< Broker hostname or IP. Required.            */
    uint16_t    port;       /**< Broker port, typically 1883.                */
    const char *username;   /**< May be "" for an anonymous broker.          */
    const char *password;   /**< May be "" for an anonymous broker.          */
    const char *device_id;  /**< Stable id, e.g. "ha_epdinky_a1b2c3" - see
                                  ha_mqtt_device_id().                       */
    uint32_t    state_max_age_s; /**< Published as every sensor/binary_sensor's
                                       `expire_after`: how long Home Assistant
                                       should keep showing a reading before
                                       marking it unavailable. The caller
                                       should set this generously above the
                                       refresh interval (main.c uses roughly
                                       2x it) so one retried or delayed cycle
                                       doesn't flap the entities unavailable -
                                       0 disables expiry (not recommended). */
} ha_mqtt_config_t;

/** Everything this cycle has to report. */
typedef struct {
    float    battery_voltage_v;
    bool     battery_present;      /**< Gates both battery fields below - see
                                         ha_battery.h: no fabricated values. */
    float    battery_percent;
    bool     battery_charging;
    int      wifi_rssi_dbm;
    const char *last_refresh_iso8601; /**< RFC3339 UTC, or "" if the clock
                                            was never set - see ha_time.h.  */
} ha_mqtt_state_t;

/**
 * @brief  Derive this device's stable MQTT identity from the station MAC.
 *
 * "ha_epdinky_" followed by the last 3 MAC bytes in lowercase hex, e.g.
 * "ha_epdinky_a1b2c3". Used as both the MQTT client id and the Home
 * Assistant device/node id, so re-provisioning the same physical board
 * (new Wi-Fi network, new broker) still merges into the same HA device
 * rather than creating a duplicate.
 */
esp_err_t ha_mqtt_device_id(char *out, size_t len);

/**
 * @brief  Run one cycle's worth of MQTT work: connect, announce, publish,
 *         listen briefly for a refresh command, disconnect.
 *
 * Best-effort throughout - telemetry is not required for the dashboard image
 * fetch that follows it in main.c's cycle, so a broker that is unreachable
 * costs this cycle's telemetry, not the whole cycle. See main.c for how a
 * failure here is surfaced (a brief status screen, not an aborted cycle).
 *
 * @param  cfg               broker connection details.
 * @param  state             current readings to publish.
 * @param  command_window_s  how long to listen for a refresh-button press
 *                            after publishing (CONFIG_HA_MQTT_COMMAND_WINDOW_S).
 * @param  out_refresh_requested  set true if a refresh command arrived during
 *                            the listen window; left false otherwise (and on
 *                            failure). May be NULL if the caller does not
 *                            need it (e.g. a build with no button use for it).
 *
 * @return ESP_OK only if the connect, the publishes and the listen window all
 *         completed; any other value means at least the connect failed and
 *         nothing was published.
 */
esp_err_t ha_mqtt_run_cycle(const ha_mqtt_config_t *cfg,
                            const ha_mqtt_state_t  *state,
                            uint32_t                command_window_s,
                            bool                   *out_refresh_requested);

/**
 * @brief  Open a background listener on the refresh-button command topic,
 *         for the idle wait between cycles (see ha_sleep_wait()'s
 *         external_wake parameter).
 *
 * Unlike ha_mqtt_run_cycle() this does not publish discovery or state - that
 * already happened this cycle - and it does not block: it starts esp-mqtt's
 * own task and returns immediately. Once connected it subscribes to the
 * command topic, and sets *out_refresh_requested (never clears it - that's
 * the caller's job) the moment a press arrives, so the caller's own poll
 * loop is what actually acts on it. Safe to call again after
 * ha_mqtt_listen_stop() - it is not safe to call while a listener from a
 * previous call is still running.
 *
 * @param  cfg  broker connection details - the same ones ha_mqtt_run_cycle()
 *              used this cycle.
 * @param  out_refresh_requested  set true from the MQTT task's context (not
 *              the caller's) on a matching command; must not be NULL and must
 *              outlive the listener.
 *
 * @return ESP_OK once the client has started connecting (not once it has
 *         actually connected - a broker that never accepts the connection
 *         just means no press ever arrives, not a hang); an error only for a
 *         bad argument or if starting the client itself failed.
 */
esp_err_t ha_mqtt_listen_start(const ha_mqtt_config_t *cfg, volatile bool *out_refresh_requested);

/** @brief  Tear down the listener started by ha_mqtt_listen_start().
 *  Safe to call even if start failed or was never called. */
void ha_mqtt_listen_stop(void);

#ifdef __cplusplus
}
#endif
