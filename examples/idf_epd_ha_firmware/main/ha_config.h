/*
 * Build-time defaults and NVS layout for the Home Assistant e-paper firmware.
 *
 * This firmware has no server protocol of its own to describe - unlike the
 * TRMNL port's trmnl_config.h, there is no vendor API to be byte-compatible
 * with. What is fixed at build time lives here (firmware identity, panel
 * geometry, NVS key names and buffer sizes); the actual broker address,
 * credentials, dashboard URL and token are runtime values collected once by
 * the captive portal and stored in NVS - see main/portal/ha_portal.c. There is
 * deliberately no `ha_credentials.h`.
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

/*
 * Panel geometry. Unlike the TRMNL port, nothing here has to match a fixed
 * server's rendering assumptions - the dashboard-image render service the
 * user points this firmware at is themselves responsible for producing a PNG
 * at the panel's native resolution, so the reported size follows the Kconfig
 * panel choice directly (see main/Kconfig.projbuild's HA_PANEL choice and
 * main/ha_display.c).
 */
#if CONFIG_HA_PANEL_ED133UT2
#define HA_DISPLAY_WIDTH  2200
#define HA_DISPLAY_HEIGHT 1650
#else
#define HA_DISPLAY_WIDTH  1872
#define HA_DISPLAY_HEIGHT 1404
#endif

/* ── NVS keys ─────────────────────────────────────────────────────────────── */

/* NVS namespace and key names are both capped at 15 characters; every key
 * below is within that. */
#define HA_NVS_NAMESPACE "ha_epdinky"

#define HA_NVS_WIFI_SSID   "wifi_ssid"
#define HA_NVS_WIFI_PASS   "wifi_pass"
#define HA_NVS_MQTT_HOST   "mqtt_host"
#define HA_NVS_MQTT_PORT   "mqtt_port"
#define HA_NVS_MQTT_USER   "mqtt_user"
#define HA_NVS_MQTT_PASS   "mqtt_pass"
#define HA_NVS_IMAGE_URL   "image_url"
#define HA_NVS_HA_TOKEN    "ha_token"
#define HA_NVS_REFRESH_S   "refresh_s"

/* Set (to 1) by the button's "reconfigure" gesture, cleared once the stage-2
 * form is actually submitted. Unlike needs_ha_config() (main.c), this does
 * not depend on any field being empty - it exists so "reconfigure" can force
 * the stage-2 config server back up without erasing the settings it collects,
 * so the form can be pre-filled with what's already stored instead of asking
 * for everything again. */
#define HA_NVS_FORCE_CFG   "force_cfg"

/* wifi_config_t.sta.ssid/password are 32 and 64 bytes; +1 for the terminator
 * each matches what esp_wifi actually accepts. */
#define HA_WIFI_SSID_MAX 64
#define HA_WIFI_PASS_MAX 128

/* Home Assistant's Mosquitto broker add-on auto-generates a 64-character
 * password for its internal "homeassistant" account - the account most
 * users will actually point this firmware at - so 64 here (as a buffer size,
 * i.e. a 63-character limit after the null terminator) was one character too
 * small to hold it without silent truncation. 128 leaves headroom. */
#define HA_MQTT_HOST_MAX 128
#define HA_MQTT_USER_MAX 128
#define HA_MQTT_PASS_MAX 128

/* The dashboard-image URL, e.g. a headless-browser screenshot service. */
#define HA_IMAGE_URL_MAX 256

/* Home Assistant long-lived access tokens are JWTs, typically 150-200 bytes;
 * this leaves generous headroom. */
#define HA_TOKEN_MAX 512

/* ── Runtime limits ───────────────────────────────────────────────────────── */

#define HA_REFRESH_S_MIN     60
#define HA_REFRESH_S_MAX     86400

/* A 1872x1404 (or 2200x1650) dashboard PNG comfortably fits well under this;
 * it exists to bound an unbounded/misconfigured server response, not because
 * real images approach it. */
#define HA_IMAGE_MAX_BYTES (1024 * 1024)

#define HA_WIFI_CONNECT_TIMEOUT_MS 30000
#define HA_HTTP_TIMEOUT_MS         20000
#define HA_SNTP_TIMEOUT_MS         10000

/*
 * How soon to try again after a cycle fails (Wi-Fi, MQTT connect, or the
 * image fetch/decode) rather than waiting out the full refresh interval.
 * There is no server-driven back-off ladder here as there is in the
 * trmnl-firmware example - nothing in this firmware's protocol has a notion
 * of "come back sooner, the content isn't ready yet" - so a single fixed
 * retry interval, shorter than the normal cadence but not so short it spins
 * on a broker or render service that is going to stay down for a while, is
 * the whole policy.
 */
#define HA_RETRY_INTERVAL_S 60

/* Upstream SNTP pool; no timezone is set - everything here is UTC, since MQTT
 * timestamps and logs are machine-read, not displayed. */
#define HA_SNTP_SERVER "pool.ntp.org"

#endif /* HA_CONFIG_H */
