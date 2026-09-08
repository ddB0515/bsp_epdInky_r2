/*
 * Wi-Fi provisioning: a SoftAP + captive portal that collects an SSID and
 * password and saves them to NVS - the only place Wi-Fi credentials live.
 * Runs whenever NVS has none: a fresh device, or one just cleared by the
 * ClearWifi button gesture.
 */
#pragma once

#include "esp_err.h"

#include "bsp/epdinky_p4_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Run the provisioning portal until credentials are saved or it
 *         times out.
 *
 * @param  board  handles for the peripherals that have to be quietened before
 *                a timeout sleep - the same ones trmnl_sleep.c's deep-sleep
 *                path takes, for the same reason. Only the PMIC is used here;
 *                the SD cache and clock aren't up yet at the point this runs.
 *
 * On success this does not return: the device reboots from inside the
 * "/connect" handler so the next boot starts clean in station mode. On a
 * 15-minute inactivity timeout - the same "prevent dead batteries" reasoning
 * upstream's portal uses - it tears everything down and deep-sleeps instead
 * of holding the radio up forever, so the portal comes back on the next
 * timed wake rather than draining the battery indefinitely.
 *
 * @return An error only if the AP/DNS/HTTP server themselves failed to
 *         start; the caller should fall through to the normal (empty-SSID)
 *         connect attempt, which fails onto the existing Wi-Fi retry ladder.
 */
esp_err_t trmnl_portal_run(const bsp_epdinky_handles_t *board);

#ifdef __cplusplus
}
#endif
