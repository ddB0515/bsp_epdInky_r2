/*
 * TRMNL protocol constants.
 *
 * Ported from upstream `include/config.h` (usetrmnl/trmnl-firmware @ 5e8fb5e,
 * FW 1.8.16), with the Arduino board #ifdef ladder removed. Only the values
 * that describe the *server protocol* live here; anything about this board's
 * hardware belongs in bsp/config.h.
 */

#ifndef TRMNL_CONFIG_H
#define TRMNL_CONFIG_H

/* ── Firmware identity ────────────────────────────────────────────────────── */

/*
 * Reported to the server in the FW-Version header and used by /api/display to
 * decide whether to offer an update. Tracks the upstream release this port was
 * written against; bump it when the port picks up a newer upstream.
 */
#define TRMNL_FW_MAJOR 1
#define TRMNL_FW_MINOR 8
#define TRMNL_FW_PATCH 16

#define TRMNL_STR2(x) #x
#define TRMNL_STR(x)  TRMNL_STR2(x)
#define TRMNL_FW_VERSION_STRING \
    TRMNL_STR(TRMNL_FW_MAJOR) "." TRMNL_STR(TRMNL_FW_MINOR) "." TRMNL_STR(TRMNL_FW_PATCH)

/*
 * The Model header. "x" is upstream's TRMNL X: a parallel-bus e-ink device with
 * a 1872x1404 10.3" panel (device_list[] in src/display.cpp, BB_PANEL_TRMNL_X).
 *
 * That is exactly the geometry of the ED103TC2, the panel trmnl.app itself
 * renders for, so advertising "x" gets us server-side rendering already
 * tuned for this size rather than an 800x480 image upscaled by a factor of
 * 2.3.
 */
#define TRMNL_DEVICE_MODEL "x"

/*
 * Panel geometry advertised in the Width / Height headers - what the server
 * is told, not necessarily what's physically connected. Deliberately does
 * NOT follow the TRMNL_PANEL Kconfig choice: trmnl.app only renders for
 * resolutions it knows, so a bench panel it doesn't support (ED133UT2) still
 * gets reported as this size to keep getting real content, and
 * trmnl_display_init() scales what arrives to fit whatever panel is
 * actually connected. See the doc comment there.
 */
#define TRMNL_DISPLAY_WIDTH  1872
#define TRMNL_DISPLAY_HEIGHT 1404

/* ── Server ───────────────────────────────────────────────────────────────── */

#define TRMNL_API_BASE_URL     "https://trmnl.app"
#define TRMNL_API_SETUP_PATH   "/api/setup"
#define TRMNL_API_DISPLAY_PATH "/api/display"
#define TRMNL_API_LOG_PATH     "/api/log"

/* Upstream's inactivity timeout while streaming an image body. */
#define TRMNL_IMAGE_STREAM_INACTIVITY_TIMEOUT_MS 15000

/* ── Refresh ──────────────────────────────────────────────────────────────── */

#define TRMNL_SLEEP_TIME_DEFAULT_S 900  /* used until the server says otherwise */
#define TRMNL_SLEEP_TIME_MIN_S     5
#define TRMNL_SLEEP_TIME_MAX_S     86400

/* ── Temperature profiles (server-selectable waveform hints) ──────────────── */

#define TRMNL_TEMP_PROFILE_DEFAULT 0
#define TRMNL_TEMP_PROFILE_A       1
#define TRMNL_TEMP_PROFILE_B       2
#define TRMNL_TEMP_PROFILE_C       3

/* ── NVS keys ─────────────────────────────────────────────────────────────── */

/*
 * Byte-for-byte the keys upstream stores in Arduino Preferences. Kept identical
 * so a device provisioned by upstream firmware and one provisioned by this port
 * are interchangeable, and so the upstream source stays a usable reference.
 *
 * NVS keys are limited to 15 characters; every key below is within that.
 */
#define TRMNL_NVS_NAMESPACE "trmnl"

#define TRMNL_NVS_API_KEY            "api_key"
#define TRMNL_NVS_API_URL            "api_url"
#define TRMNL_NVS_FRIENDLY_ID        "friendly_id"
#define TRMNL_NVS_HOSTNAME           "hostname"
#define TRMNL_NVS_TEMP_PROFILE       "temp_profile"
#define TRMNL_NVS_DEVICE_REGISTERED  "plugin"
#define TRMNL_NVS_SPECIAL_FUNCTION   "sf"
#define TRMNL_NVS_FILENAME           "filename"
#define TRMNL_NVS_LAST_SLEEP_TIME    "last_sleep"
#define TRMNL_NVS_API_RETRY_COUNT    "retry_count"
#define TRMNL_NVS_WIFI_RETRY_COUNT   "wifi_retry"
#define TRMNL_NVS_LAST_OTA           "last_ota"
#define TRMNL_NVS_WIFI_SSID          "wifi_ssid"
#define TRMNL_NVS_WIFI_PASS          "wifi_pass"

/* wifi_config_t.sta.ssid/password are 32 and 64 bytes; +1 for the
 * terminator each matches what esp_wifi actually accepts. */
#define TRMNL_WIFI_SSID_MAX 33
#define TRMNL_WIFI_PASS_MAX 64

/* These two live in upstream's refresh_interval.h rather than its config.h,
 * but they are Preferences keys like the rest and belong with them. */
#define TRMNL_NVS_REFRESH_RATE       "refresh_rate"
#define TRMNL_NVS_FAST_POLLS         "fast_polls"

#endif /* TRMNL_CONFIG_H */
