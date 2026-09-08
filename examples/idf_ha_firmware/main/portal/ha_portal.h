/*
 * Provisioning, in two stages.
 *
 * Stage 1 - ha_portal_run_wifi(): a SoftAP + captive portal that collects
 * only the Wi-Fi SSID/password, then saves them to NVS and reboots. Runs
 * whenever NVS has no Wi-Fi SSID: a fresh device, or one just fully cleared
 * by the button's factory-reset gesture (held > 20 s).
 *
 * Stage 2 - ha_portal_run_config(): once the device has joined that network
 * and has a real DHCP address, a plain HTTP server (no SoftAP, no DNS
 * hijack - the user just browses to it) collects the Home Assistant
 * WebSocket host/port and long-lived access token, then saves them and
 * reboots. Runs whenever the host or token is not yet set: after stage 1
 * the first time, or after the button's "reconfigure" gesture (held 5-20 s)
 * has cleared just those fields.
 *
 * Adapted from examples/idf_epd_ha_firmware/main/portal/ha_portal.h. The
 * biggest difference from that e-paper version: neither function here blocks
 * or times out. This device is mains-powered and always-on, so there is no
 * battery-drain reason to tear the portal down and deep-sleep if nobody
 * configures it right away - each function just starts its server and shows
 * its setup screen on the panel, then returns immediately. main.c's ordinary
 * button-poll loop keeps running underneath it exactly as it does once the
 * dashboard is up; the device just sits on the setup screen, fully
 * responsive, for as long as it takes.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start the Wi-Fi-only provisioning portal (SoftAP + DNS + HTTP) and
 *         show its setup screen. Non-blocking.
 *
 * On a successful submission the device reboots from inside the "/connect"
 * handler (a background httpd task), so the next boot starts clean in
 * station mode.
 *
 * @return An error only if the AP/DNS/HTTP server themselves failed to
 *         start.
 */
esp_err_t ha_portal_run_wifi(void);

/**
 * @brief  Start the Home Assistant config server (plain HTTP, on the
 *         existing station network) and show its setup screen. Non-blocking.
 *
 * Call only once station Wi-Fi is already connected - this does not bring up
 * Wi-Fi itself.
 *
 * Same non-blocking, no-timeout contract as ha_portal_run_wifi(): reboots
 * on save, from inside the "/connect" handler.
 *
 * @return An error only if the HTTP server itself failed to start.
 */
esp_err_t ha_portal_run_config(void);

#ifdef __cplusplus
}
#endif
