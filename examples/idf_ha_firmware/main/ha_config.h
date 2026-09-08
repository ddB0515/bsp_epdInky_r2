/*
 * Build-time defaults and NVS layout for the Home Assistant TFT dashboard
 * firmware.
 *
 * Same philosophy as examples/idf_epd_ha_firmware/main/ha_config.h: nothing
 * installation-specific is compiled in. The Wi-Fi network, the Home
 * Assistant URL and its long-lived access token are runtime values collected
 * once by the provisioning portal and stored in NVS - see
 * main/portal/ha_portal.c. There is deliberately no `ha_credentials.h`.
 *
 * The dashboard's tile list (which entities to show) is the one thing that
 * *is* compiled in for v1 - see main/ha_dashboard_config.h - since it is
 * edited per installation and reflashed, not provisioned over the network.
 */
#ifndef HA_CONFIG_H
#define HA_CONFIG_H

/* ── Firmware identity ────────────────────────────────────────────────────── */

#define HA_FW_MAJOR 1
#define HA_FW_MINOR 0
#define HA_FW_PATCH 0

#define HA_STR2(x) #x
#define HA_STR(x)  HA_STR2(x)
#define HA_FW_VERSION_STRING \
    HA_STR(HA_FW_MAJOR) "." HA_STR(HA_FW_MINOR) "." HA_STR(HA_FW_PATCH)

/* ── NVS keys ─────────────────────────────────────────────────────────────── */

/* NVS namespace and key names are both capped at 15 characters; every key
 * below is within that. A different namespace from examples/idf_epd_ha_firmware
 * ("ha_epdinky") so the two firmwares' settings never collide if the same
 * flash is ever reused between them. */
#define HA_NVS_NAMESPACE "ha_tft"

#define HA_NVS_WIFI_SSID "wifi_ssid"
#define HA_NVS_WIFI_PASS "wifi_pass"
#define HA_NVS_WS_HOST   "ws_host"
#define HA_NVS_WS_PORT   "ws_port"
#define HA_NVS_WS_TOKEN  "ws_token"

/* Set (to 1) by the button's "reconfigure" gesture, cleared once the stage-2
 * form is actually submitted - same purpose as the e-paper firmware's own
 * HA_NVS_FORCE_CFG: force the stage-2 config server back up without erasing
 * what it already collected, so the form can be pre-filled instead of asking
 * for everything again. */
#define HA_NVS_FORCE_CFG "force_cfg"

/* wifi_config_t.sta.ssid/password are 32 and 64 bytes; +1 for the terminator
 * each matches what esp_wifi actually accepts. */
#define HA_WIFI_SSID_MAX 64
#define HA_WIFI_PASS_MAX 128

/* Home Assistant hostnames/IPs are short; this leaves generous headroom for
 * something like a long .duckdns.org or Nabu Casa hostname. */
#define HA_WS_HOST_MAX 128

/* Home Assistant long-lived access tokens are JWTs, typically 150-200 bytes;
 * this leaves generous headroom. */
#define HA_WS_TOKEN_MAX 512

/* ── Runtime limits ───────────────────────────────────────────────────────── */

#define HA_WIFI_CONNECT_TIMEOUT_MS 30000

/* Home Assistant's default un-proxied port. */
#define HA_WS_PORT_DEFAULT CONFIG_HA_WS_DEFAULT_PORT

#endif /* HA_CONFIG_H */
