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
 * hijack - the user just browses to it) collects the MQTT broker, the
 * dashboard image URL/token and the refresh interval, then saves them and
 * reboots. Runs whenever either the MQTT host or the dashboard URL is not
 * yet set: after stage 1 the first time, or after the button's
 * "reconfigure" gesture (held 5-20 s) has cleared just those fields.
 *
 * Adapted from the trmnl-firmware example's captive portal (same SoftAP +
 * DNS + web-form-to-NVS shape for stage 1); stage 2 has no TRMNL equivalent -
 * that example never needed a second, network-reachable configuration step.
 */
#pragma once

#include "esp_err.h"

#include "bsp/epdinky_p4_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Run the Wi-Fi-only provisioning portal until a network is saved or
 *         it times out.
 *
 * @param  board  handles for the peripherals that have to be quietened before
 *                a timeout sleep. Only the PMIC is used here; the clock isn't
 *                up yet at the point this runs.
 *
 * On success this does not return: the device reboots from inside the
 * "/connect" handler so the next boot starts clean in station mode. On a
 * 15-minute inactivity timeout it tears everything down and deep-sleeps
 * instead of holding the radio up forever, so the portal comes back on the
 * next timed wake rather than draining the battery indefinitely.
 *
 * @return An error only if the AP/DNS/HTTP server themselves failed to
 *         start; the caller should fall through to the normal (empty-SSID)
 *         connect attempt, which fails onto the existing Wi-Fi retry path.
 */
esp_err_t ha_portal_run_wifi(const bsp_epdinky_handles_t *board);

/**
 * @brief  Run the MQTT/Home Assistant config server until settings are saved
 *         or it times out.
 *
 * Call only once station Wi-Fi is already connected - this starts a plain
 * HTTP server on the existing station interface, it does not bring up
 * Wi-Fi itself. Shows the "browse to http://<ip>/" screen on the panel.
 *
 * @param  board  handles for the peripherals that have to be quietened before
 *                a timeout sleep.
 *
 * Same non-return/timeout contract as ha_portal_run_wifi(): reboots on save,
 * deep-sleeps and retries after 15 minutes of inactivity.
 *
 * @return An error only if the HTTP server itself failed to start.
 */
esp_err_t ha_portal_run_config(const bsp_epdinky_handles_t *board);

#ifdef __cplusplus
}
#endif
