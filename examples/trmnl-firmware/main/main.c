/*
 * TRMNL firmware for the epdInky ESP32-P4/C6 rev.2 - phase 3: the cycle.
 *
 * WHAT THIS BUILD DOES
 *
 *   1. brings the board up - PMIC, panel, framebuffer, expander, RTC, button -
 *      and then runs a cycle:
 *   2. joins Wi-Fi through the BSP (esp-hosted -> on-board ESP32-C6),
 *   3. sets the clock: RV-3028 at boot, SNTP once a day, written back,
 *   4. registers with trmnl.app via GET /api/setup, keyed by the station MAC,
 *   5. asks for a frame via GET /api/display,
 *   6. serves that frame from the SD card if it is already there, downloads it
 *      otherwise, and skips the refresh entirely when it is the frame already
 *      on the glass,
 *   7. waits refresh_rate seconds and goes round again.
 *
 * Every failure arm sets a back-off interval instead of stopping, so a device
 * that cannot reach the network or the server keeps retrying on the ladder in
 * trmnl_refresh.c rather than needing a reset.
 *
 * Step 7 is either a deep sleep or an idle wait, chosen at build time; see
 * trmnl_sleep.h for what each costs. In deep-sleep builds "goes round again" is
 * literally a fresh boot, so app_main() below runs once per cycle and the loop
 * at the end of it only ever iterates when idling.
 *
 * In idle builds, a press on the one button (GPIO35) also ends the wait early -
 * see trmnl_button.h for how a short/double/long press is told apart, and
 * run_cycle()'s switch on trmnl_sleep_button_gesture() for what each one does:
 * a double-click reports the dashboard-configured special_function to the
 * server, except add_wifi, which launches the provisioning portal directly
 * (it needs a device-local capability the server-reported header cannot
 * provide); a long press is the ClearWifi gesture - see handle_clear_wifi().
 *
 * Wi-Fi credentials live only in NVS - there is no compiled-in fallback. A
 * device with nothing in NVS yet, or one just cleared via ClearWifi, drops
 * into trmnl_portal_run() (main/portal/): a SoftAP + captive portal that
 * saves whatever the user submits and reboots.
 *
 * What is still missing: the update/OTA arm (deferred by decision, not
 * forgotten. reset_firmware is handled - see handle_reset_firmware().
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "bsp/epdinky_p4_board.h"

#include "trmnl_api.h"
#include "trmnl_battery.h"
#include "trmnl_button.h"
#include "trmnl_cache.h"
#include "trmnl_config.h"
#include "trmnl_display.h"
#include "trmnl_http.h"
#include "trmnl_persist.h"
#include "trmnl_portal.h"
#include "trmnl_refresh.h"
#include "trmnl_sleep.h"
#include "trmnl_time.h"

/*
 * Optional: only needed to point at a self-hosted TRMNL server
 * (TRMNL_API_BASE_URL_OVERRIDE below). Wi-Fi is never read from here - see
 * the file header comment.
 */
#if __has_include("trmnl_credentials.h")
#include "trmnl_credentials.h"
#endif

#ifdef TRMNL_API_BASE_URL_OVERRIDE
#define TRMNL_BASE_URL TRMNL_API_BASE_URL_OVERRIDE
#else
#define TRMNL_BASE_URL TRMNL_API_BASE_URL
#endif

static const char *TAG = "trmnl";

/* A 1872x1404 frame is 329 KB as raw 1-bpp; 1 MB covers a BMP header, any
 * compression the server chooses not to apply, and a firmware-sized surprise. */
#define TRMNL_IMAGE_MAX_BYTES (1024 * 1024)

#define TRMNL_WIFI_CONNECT_TIMEOUT_MS 30000

/* SNTP runs at most once a day, so a generous window costs nothing. */
#define TRMNL_SNTP_TIMEOUT_MS 10000

/* ===========================================================================
 * State that outlives a cycle
 *
 * In a deep-sleep build a "cycle" is a whole boot, so anything here that has to
 * carry over has to be told where to live. Three lifetimes are in play:
 *
 *   - plain statics, valid within one boot: the identity strings below, which
 *     are re-read from NVS or re-derived on each one anyway;
 *   - RTC_DATA_ATTR, retained across a deep sleep and reset by a power cycle:
 *     the counters and flags, whose initialisers here are therefore exactly the
 *     right cold-boot values;
 *   - NVS, retained across everything: the refresh interval (trmnl_refresh.c),
 *     the credentials, and the name of the frame on the glass.
 *
 * The counters are in RTC memory rather than NVS on purpose. They change on
 * every failure, and a device that has been offline for a day would otherwise
 * have written the flash a hundred times to remember a number that stops
 * mattering the moment the network comes back.
 * ========================================================================= */

static bsp_epdinky_handles_t s_board;
static bsp_epdinky_config_t  s_board_cfg;

static char s_mac[18];
static char s_api_key[TRMNL_API_KEY_MAX];
static char s_friendly_id[TRMNL_FRIENDLY_ID_MAX];

/* Consecutive failures, driving trmnl_refresh_apply_*_retry(). Reset by a
 * success of the same kind. */
RTC_DATA_ATTR static uint8_t s_wifi_attempts;
RTC_DATA_ATTR static uint8_t s_api_attempts;

/*
 * Upstream's bUsedCachedImage: whether the *previous* pass served its image
 * from storage rather than downloading it. Read when the next /api/display
 * request is built, which is before this pass knows its own answer - so the
 * Image-Cached header is always one cycle behind. That is upstream's behaviour
 * and the server treats it as a hint, not a fact.
 */
RTC_DATA_ATTR static bool s_used_cached_image;

/* Wall-clock of the last completed cycle, for the Prev-Wake header. */
RTC_DATA_ATTR static uint32_t s_prev_wake_time;

/* /api/log's "id" field. Upstream's own semantics for it aren't documented
 * anywhere reachable from this port; a monotonically increasing counter that
 * survives deep sleep (only a power cycle resets it) is enough to tell two
 * entries apart, which is all a caller of /api/log could reasonably want. */
RTC_DATA_ATTR static uint32_t s_log_id;

/*
 * Whether the next image should be preceded by full black/white clean cycles.
 * True on a cold boot and after any status screen, because large text ghosts
 * badly behind a photograph; false between routine frames, where a clean costs
 * about a second of visible flashing every quarter of an hour for nothing.
 *
 * The initialiser is what makes a power cycle clean and a timed wake not, which
 * is also how upstream behaves: it forces a refresh on any wake that was not
 * the timer.
 */
RTC_DATA_ATTR static bool s_needs_clean = true;

/* ===========================================================================
 * Credentials
 *
 * NVS only - no compiled-in fallback. Nothing here means app_main() drops
 * into trmnl_portal_run() instead of ever calling these.
 * ========================================================================= */

static const char *wifi_ssid(void)
{
    static char ssid[TRMNL_WIFI_SSID_MAX];
    trmnl_persist_get_str(TRMNL_NVS_WIFI_SSID, ssid, sizeof(ssid));
    return ssid;
}

static const char *wifi_password(void)
{
    static char password[TRMNL_WIFI_PASS_MAX];
    trmnl_persist_get_str(TRMNL_NVS_WIFI_PASS, password, sizeof(password));
    return password;
}

/* ===========================================================================
 * Device identity
 * ========================================================================= */

/*
 * The ID header. Upstream sends Arduino's WiFi.macAddress(), which is the
 * station MAC in uppercase colon-separated form - the server keys the device
 * record on this string, so the formatting matters.
 */
static esp_err_t device_mac_string(char *out, size_t len)
{
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
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
 * What is currently on the glass
 *
 * Persisted rather than kept in RAM because the panel holds its image without
 * power: after a reset the frame is still there, and the firmware needs to know
 * that so it does not redraw an identical image on every reboot.
 * ========================================================================= */

static bool displayed_matches(const char *filename)
{
    char shown[TRMNL_FILENAME_MAX] = { 0 };
    if (trmnl_persist_get_str(TRMNL_NVS_FILENAME, shown, sizeof(shown)) != ESP_OK) {
        return false;
    }
    return shown[0] != '\0' && strcmp(shown, filename) == 0;
}

static void displayed_set(const char *filename)
{
    if (filename != NULL && filename[0] != '\0') {
        trmnl_persist_set_str(TRMNL_NVS_FILENAME, filename);
    }
}

/*
 * The special_function field is not a command, despite the name. It is the
 * action the user has configured for the button on the dashboard, and upstream
 * acts on it only after a double-click wake, having read it back out of
 * Preferences; on a timer wake bl.cpp calls writeSpecialFunction() and does
 * nothing else. "identify" is the dashboard default, so a device that has never
 * been configured reports it on every single cycle.
 *
 * Storing it is therefore the whole of the correct behaviour here - dispatch
 * belongs with the button gesture, not with the frame update. Upstream in fact
 * compiles the double-click path out entirely for BOARD_TRMNL_X, which is the
 * model this port advertises.
 *
 * A double-click now reads this back out of NVS and reports it to the server -
 * see the switch on trmnl_sleep_button_gesture() above the /api/display call in
 * run_cycle(). This function only ever stores; it is never the one dispatching.
 *
 * Guarded so the log stays quiet: it says something only when the action
 * actually changes.
 */
static void remember_special_function(trmnl_special_function_t sf)
{
    if (trmnl_persist_set_u32_if_changed(TRMNL_NVS_SPECIAL_FUNCTION, (uint32_t)sf)) {
        ESP_LOGI(TAG, "button action is now \"%s\"", trmnl_special_function_str(sf));
    }
}

/* ===========================================================================
 * Error reporting
 * ========================================================================= */

/*
 * Best-effort /api/log submission for a failure report - upstream's own
 * Log_error_submit() at the equivalent points in bl.cpp's error arms.
 *
 * Deliberately not upstream's whole story: it keeps a store-and-batch queue
 * (lib/trmnl/src/stored_logs.cpp) in NVS so a submission that fails during an
 * outage still reaches the server once connectivity returns, sent as one
 * request per boot with everything gathered since. This port has no such
 * queue - a submission that fails here is simply lost, not queued for next
 * cycle. That is a real gap against upstream, not a simplification with no
 * cost, and it is worth adding if failures during real outages turn out
 * common enough to want a paper trail for. Nothing today depends on it: this
 * still gets a failure onto the dashboard immediately whenever the network
 * is actually up, which is the common case a failure this port can even
 * observe (report_failure() itself is never reached before Wi-Fi exists,
 * except for the "no Wi-Fi" failure below, which this guards against).
 */
static void submit_log(const char *heading, const char *body, const char *file, int line)
{
    if (!bsp_wifi_is_connected() || s_api_key[0] == '\0') {
        return;
    }

    char message[128];
    snprintf(message, sizeof(message), "%s: %s", heading, body);

    trmnl_battery_status_t battery;
    trmnl_battery_read(&battery);

    trmnl_log_entry_t entry = {
        .wifi_rssi_level  = (int8_t)current_rssi(),
        .wifi_status      = "Connected", /* the guard above already requires it */
        .refresh_rate     = trmnl_refresh_seconds(),
        .sleep_duration   = (trmnl_time_is_valid() && s_prev_wake_time != 0)
                            ? (uint32_t)(time(NULL) - (time_t)s_prev_wake_time) : 0,
        .firmware_version = TRMNL_FW_VERSION_STRING,
        .special_function = trmnl_special_function_str(
            (trmnl_special_function_t)trmnl_persist_get_u32(TRMNL_NVS_SPECIAL_FUNCTION, TRMNL_SF_NONE)),
        .battery_voltage  = battery.present ? battery.voltage_v : 4.10f,
        .wake_reason      = trmnl_sleep_update_source(),
        .free_heap_size   = esp_get_free_heap_size(),
        .max_alloc_size   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        .log_id           = ++s_log_id,
        .timestamp        = trmnl_time_is_valid() ? time(NULL) : 0,
        .source_file      = file,
        .source_line      = line,
        .message          = message,
        .level            = TRMNL_LOG_ERROR,
        .has_retry        = false,
    };

    trmnl_log_inputs_t log_in = { .base_url = TRMNL_BASE_URL, .mac = s_mac, .api_key = s_api_key };
    esp_err_t err = trmnl_api_log(&log_in, &entry, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "log submission failed: %s", esp_err_to_name(err));
    }
}

/*
 * Draw a status screen, but only on the first failure of a run.
 *
 * The back-off ladder retries every 15 s at first, and a status screen is a
 * full clean plus a GC16 - about four seconds of flashing panel. Repeating that
 * on every retry would make a transient server problem look like a fault and
 * would cost far more energy than the retry itself. The first one is worth
 * showing; the rest go to the log - and, per submit_log(), the first is also
 * the only one worth telling the server about.
 */
static void report_failure_impl(uint8_t attempt, uint32_t next_s,
                                const char *heading, const char *body,
                                const char *file, int line)
{
    ESP_LOGE(TAG, "%s: %s (attempt %u, retrying in %" PRIu32 " s)",
             heading, body, attempt, next_s);

    if (attempt <= 1) {
        char footer[64];
        snprintf(footer, sizeof(footer), "retrying in %" PRIu32 " s", next_s);
        trmnl_display_message(heading, body, footer);
        s_needs_clean = true;

        submit_log(heading, body, file, line);
    }
}

/* Captures the call site the way upstream's own Log_* macros do
 * (trmnl_log.h's _LOG_IMPL), so submit_log() can report where a failure
 * actually happened without every call site passing __FILE__/__LINE__ by hand. */
#define report_failure(attempt, next_s, heading, body) \
    report_failure_impl((attempt), (next_s), (heading), (body), __FILE__, __LINE__)

/* ===========================================================================
 * Image fetch and display
 * ========================================================================= */

/*
 * Identify the payload from its first bytes, using exactly the tests upstream's
 * src/display.cpp applies in the same order. PNG (plugin frames) and BMP
 * (system screens, e.g. the pre-claim setup screen) are both decodable; the
 * rest exist so that an unexpected format names itself in the log instead of
 * arriving as an anonymous decode failure.
 */
static const char *identify_image(const uint8_t *b, size_t len)
{
    if (len >= 4 && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) {
        return "PNG (decoded in-tree over the boot ROM's inflate)";
    }
    if (len >= 2 && b[0] == 0xFF && b[1] == 0xD8) {
        return "JPEG (not supported; the P4 has a hardware decoder if needed)";
    }
    /* BB_BITMAP_MARKER is 0xBBBF read as a little-endian uint16. */
    if (len >= 2 && b[0] == 0xBF && b[1] == 0xBB) {
        return "G5-compressed BB_BITMAP (not supported)";
    }
    if (len >= 2 && b[0] == 'B' && b[1] == 'M') {
        return "BMP (1bpp only, decoded in-tree - see trmnl_bmp_render())";
    }
    return "UNRECOGNISED";
}

/** Show an image and record it as the one on the glass. */
static esp_err_t show(const uint8_t *data, size_t len, const char *filename)
{
    ESP_LOGI(TAG, "format: %s", identify_image(data, len));

    esp_err_t err = trmnl_display_show_image(data, len, s_needs_clean);
    if (err != ESP_OK) {
        return err;
    }

    s_needs_clean = false;
    displayed_set(filename);
    ESP_LOGI(TAG, "frame is on the panel");
    return ESP_OK;
}

static esp_err_t download_and_show(const char *url, const char *filename)
{
    /*
     * Upstream's buildImageHeaders(): just ID and Access-Token. The URL is
     * usually a presigned object-store link that redirects, and the redirect
     * target rejects unexpected auth headers, so nothing more goes on here.
     */
    trmnl_headers_t headers;
    trmnl_headers_reset(&headers);
    trmnl_headers_add(&headers, "ID", s_mac);
    trmnl_headers_add(&headers, "Access-Token", s_api_key);

    ESP_LOGI(TAG, "--- fetching image ---");

    trmnl_http_response_t resp;
    esp_err_t err = trmnl_http_get(url, headers.items, headers.count,
                                   TRMNL_IMAGE_MAX_BYTES,
                                   TRMNL_IMAGE_STREAM_INACTIVITY_TIMEOUT_MS,
                                   &resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image fetch failed: %s", esp_err_to_name(err));
        trmnl_http_response_free(&resp);
        return err;
    }

    ESP_LOGI(TAG, "HTTP %d, Content-Type %s, %u bytes%s",
             resp.status,
             resp.content_type[0] ? resp.content_type : "(none)",
             (unsigned)resp.body_len,
             resp.truncated ? " (TRUNCATED at the cap)" : "");

    if (resp.body_len == 0) {
        ESP_LOGE(TAG, "empty body");
        trmnl_http_response_free(&resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (resp.truncated) {
        /* A partial PNG would fail in inflate anyway, but with a confusing
         * message. Say plainly that the cap was the problem. */
        ESP_LOGE(TAG, "image exceeded the %u byte cap; raise TRMNL_IMAGE_MAX_BYTES",
                 (unsigned)TRMNL_IMAGE_MAX_BYTES);
        trmnl_http_response_free(&resp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Cache before drawing. The write is cheap next to a refresh, and doing it
     * first means a power cut during the four-second GC16 still leaves a usable
     * entry for the next boot. */
    trmnl_cache_write(filename, resp.body, resp.body_len);

    err = show(resp.body, resp.body_len, filename);
    trmnl_http_response_free(&resp);
    return err;
}

/*
 * Upstream's two-step decision, kept separate because the questions are
 * different:
 *
 *   - is this file already on the card?  -> skip the download
 *   - is it the file already on the glass? -> skip the refresh as well
 *
 * The second is the one that matters for battery life. A device showing a clock
 * plugin that updates hourly wakes four times an hour to find nothing has
 * changed, and answering that with a stat() instead of a GC16 is most of the
 * difference between a week and a month of runtime.
 */
static esp_err_t update_image(const trmnl_display_response_t *r)
{
    const char *filename = r->filename;
    const bool  named    = filename[0] != '\0';

    if (named && trmnl_cache_available() && displayed_matches(filename)) {
        /*
         * The name is kept in NVS, so this test would work without a card too -
         * but by decision a card-less device downloads and refreshes every
         * cycle rather than trusting a record it cannot cross-check, so the
         * skip is gated on the cache being there at all.
         */
        ESP_LOGI(TAG, "%s is already on the panel; no refresh", filename);
        s_used_cached_image = true;
        return ESP_OK;
    }

    if (named && trmnl_cache_has(filename)) {
        uint8_t *buf = NULL;
        size_t   len = 0;

        if (trmnl_cache_read(filename, &buf, &len) == ESP_OK) {
            const esp_err_t err = show(buf, len, filename);
            heap_caps_free(buf);
            if (err == ESP_OK) {
                s_used_cached_image = true;
                return ESP_OK;
            }
            ESP_LOGW(TAG, "cached image would not render; downloading it again");
        }
        /* trmnl_cache_read() has already dropped an unreadable entry, so the
         * download below repopulates it. */
    }

    if (r->image_url[0] == '\0') {
        ESP_LOGW(TAG, "no image_url and nothing cached - nothing to draw");
        return ESP_ERR_NOT_FOUND;
    }

    s_used_cached_image = false;
    return download_and_show(r->image_url, named ? filename : "image.png");
}

/* ===========================================================================
 * Registration
 * ========================================================================= */

/**
 * @param out_fresh  set to true only when this call just completed a real
 *                    /api/setup round trip (as opposed to finding credentials
 *                    already in RAM or NVS). Never set on failure. The caller
 *                    uses this to avoid immediately following a brand-new
 *                    registration with an /api/display call that has nothing
 *                    to say yet and would overwrite the setup screen below
 *                    with an error - see run_cycle().
 */
static esp_err_t ensure_registered(bool *out_fresh)
{
    if (out_fresh != NULL) {
        *out_fresh = false;
    }

    if (s_api_key[0] != '\0') {
        return ESP_OK;
    }

    trmnl_persist_get_str(TRMNL_NVS_API_KEY,     s_api_key,     sizeof(s_api_key));
    trmnl_persist_get_str(TRMNL_NVS_FRIENDLY_ID, s_friendly_id, sizeof(s_friendly_id));

    if (s_api_key[0] != '\0') {
        ESP_LOGI(TAG, "Using stored credentials for device %s", s_friendly_id);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "--- no stored API key, registering ---");

    trmnl_setup_inputs_t in = {
        .base_url         = TRMNL_BASE_URL,
        .mac              = s_mac,
        .firmware_version = TRMNL_FW_VERSION_STRING,
        .model            = TRMNL_DEVICE_MODEL,
    };
    trmnl_setup_response_t out;
    int http_status = 0;

    esp_err_t err = trmnl_api_setup(&in, &out, &http_status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "/api/setup transport error: %s", esp_err_to_name(err));
        return err;
    }
    if (out.outcome != TRMNL_SETUP_OK) {
        ESP_LOGE(TAG, "/api/setup rejected us (HTTP %d, status %d): %s",
                 http_status, out.status, out.message);
        ESP_LOGE(TAG, "Register MAC %s on trmnl.app.", s_mac);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Registered as \"%s\"", out.friendly_id);
    strlcpy(s_api_key,     out.api_key,     sizeof(s_api_key));
    strlcpy(s_friendly_id, out.friendly_id, sizeof(s_friendly_id));

    trmnl_persist_set_str(TRMNL_NVS_API_KEY,     s_api_key);
    trmnl_persist_set_str(TRMNL_NVS_FRIENDLY_ID, s_friendly_id);

    /*
     * A device that has just registered is not yet linked to anyone's
     * trmnl.app account. /api/setup hands back a ready-made screen for
     * exactly this - "sign up with this Friendly ID" - through the same
     * server-rendered-image mechanism /api/display uses for real content, so
     * showing it is just another download_and_show() rather than anything
     * bespoke. The response's own `message` carries the actual instructions
     * ("Register at trmnl.com/start with Device ID '...'") and is drawn as a
     * caption under the image - it is the actionable part, the image is the
     * branding, so it still gets shown even if the image fails.
     *
     * Best-effort throughout: nothing here undoes a successful registration,
     * a failure just leaves less on the glass than intended.
     */
    if (out.image_url[0] != '\0') {
        ESP_LOGI(TAG, "--- fetching the setup screen ---");
        /* No extension: upstream has served this as both PNG and BMP, and
         * trmnl_image_render() sniffs the content rather than trusting a
         * name either way, so asserting one here would just be misleading. */
        if (download_and_show(out.image_url, "system_setup") == ESP_OK) {
            trmnl_display_caption(out.message);
        } else {
            ESP_LOGW(TAG, "could not show the setup screen image; showing the message alone");
            trmnl_display_message("Finish setup", out.message, "");
        }
    } else if (out.message[0] != '\0') {
        trmnl_display_message("Finish setup", out.message, "");
    }

    if (out_fresh != NULL) {
        *out_fresh = true;
    }
    return ESP_OK;
}

/* ===========================================================================
 * Reset
 *
 * reset_firmware and ClearWifi are both real commands, unlike special_function
 * - one from the dashboard (IsNeedReset), one from the button (a long press,
 * decoded in trmnl_button.c). ClearWifi erases the registration too, not just
 * the network: by decision, a long press is this device's one user-facing
 * "factory reset" gesture, so going through the setup portal afterwards
 * always re-registers from scratch - including showing the sign-up screen
 * again for a device that was never actually claimed - rather than silently
 * resuming whatever registration happened to still be sitting in NVS.
 * reset_firmware stays the narrower of the two (registration only, no Wi-Fi
 * change) because that one is server-initiated and has no reason to also
 * throw away a working network.
 * ========================================================================= */

/**
 * Erase everything that identifies this device to the server, and everything
 * that remembers what was on the glass. Shared by handle_reset_firmware() and
 * handle_clear_wifi() - the only difference between them is whether Wi-Fi
 * goes with it. Deliberately does not touch the SD image cache: the cached
 * files are just bytes keyed by filename, harmless to leave behind, and
 * re-registration may hand back the same filename anyway.
 */
static void erase_registration(void)
{
    trmnl_persist_erase(TRMNL_NVS_API_KEY);
    trmnl_persist_erase(TRMNL_NVS_FRIENDLY_ID);
    trmnl_persist_erase(TRMNL_NVS_FILENAME);
    trmnl_persist_erase(TRMNL_NVS_SPECIAL_FUNCTION);

    /*
     * s_used_cached_image is RTC_DATA_ATTR, so it survives esp_restart() same
     * as s_needs_clean below - and unlike s_needs_clean, leaving it alone is a
     * bug, not just a missed cosmetic touch-up. It drives the Image-Cached
     * header on the very next /api/display call; if it was true going into
     * this reset (the last frame before the reset was unchanged), the server
     * sees "device already has this" and answers with no filename or
     * image_url at all - while TRMNL_NVS_FILENAME above just got erased and
     * there is nothing local to fall back on. That is exactly the "Image
     * failed: ESP_ERR_NOT_FOUND" a reset must not produce.
     */
    s_used_cached_image = false;

    /* Survives the reboot (RTC_DATA_ATTR - see the note at its declaration),
     * so the first frame under the new registration gets a full clean rather
     * than a bare GC16 over whatever was left on the glass. */
    s_needs_clean = true;
}

/** Erase the registration and reboot, so the next boot re-registers as if the
 *  device were new. Leaves Wi-Fi alone - see the header comment on why. */
static void handle_reset_firmware(void)
{
    ESP_LOGW(TAG, "server asked for a reset; erasing registration and rebooting");
    trmnl_display_message("Resetting", "The server requested a reset", "rebooting...");

    erase_registration();
    esp_restart();
}

/**
 * Erase the stored Wi-Fi network *and* the registration, then reboot. The
 * next boot finds nothing in NVS and app_main() drops into
 * trmnl_portal_run() - the ClearWifi gesture's actual effect is indirect, one
 * reboot away, rather than raising the portal itself from inside a button
 * handler mid-cycle. Once the portal saves a new network and reboots again,
 * ensure_registered() finds no api_key either and calls /api/setup fresh.
 */
static void handle_clear_wifi(void)
{
    ESP_LOGW(TAG, "button held: erasing Wi-Fi and registration, rebooting into setup");
    trmnl_display_message("Wi-Fi cleared", "Rebooting into setup mode", "");

    trmnl_persist_erase(TRMNL_NVS_WIFI_SSID);
    trmnl_persist_erase(TRMNL_NVS_WIFI_PASS);
    erase_registration();

    esp_restart();
}

/* ===========================================================================
 * One pass
 *
 * Follows upstream's block scheme: connect, sync the clock, register if needed,
 * ask for a frame, show it. Every arm that gives up sets the interval before it
 * returns, so the caller never has to know why the pass ended.
 * ========================================================================= */

static void run_cycle(void)
{
    char now[24];
    trmnl_time_str(now, sizeof(now));
    ESP_LOGI(TAG, "=== cycle: %s UTC, woken by %s ===", now,
             trmnl_sleep_update_source());

    /* ---- Wi-Fi ---------------------------------------------------------- */

    if (!bsp_wifi_is_connected()) {
        ESP_LOGI(TAG, "Connecting to \"%s\"...", wifi_ssid());

        const esp_err_t err = bsp_wifi_connect(wifi_ssid(), wifi_password(),
                                               TRMNL_WIFI_CONNECT_TIMEOUT_MS);
        if (err != ESP_OK) {
            s_wifi_attempts++;
            report_failure(s_wifi_attempts,
                           trmnl_refresh_apply_wifi_retry(s_wifi_attempts),
                           "No Wi-Fi", wifi_ssid());
            return;
        }

        char ip[16] = "?";
        bsp_wifi_get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "Connected: IP %s, MAC %s, RSSI %d dBm",
                 ip, s_mac, current_rssi());
    }
    s_wifi_attempts = 0;

    /* ---- Clock ---------------------------------------------------------- */

    /* Upstream's ClockSync node. Gated, so this is a network round trip once a
     * day rather than once every fifteen minutes. */
    if (trmnl_time_sync_due()) {
        trmnl_time_sync_sntp(TRMNL_SNTP_TIMEOUT_MS);
    }

    /* ---- /api/setup ----------------------------------------------------- */

    bool freshly_registered = false;
    if (ensure_registered(&freshly_registered) != ESP_OK) {
        s_api_attempts++;
        report_failure(s_api_attempts,
                       trmnl_refresh_apply_api_retry(s_api_attempts),
                       "Not registered", s_mac);
        return;
    }
    if (freshly_registered) {
        /*
         * ensure_registered() just put the "sign up with this Friendly ID"
         * screen on the glass (if the server sent one). The account link
         * that unlocks real content happens on trmnl.app, not here, so
         * calling /api/display in the same breath would only ask a question
         * that cannot have changed yet - and an empty answer would overwrite
         * the screen just drawn with an error. Poll again soon instead
         * (the same ladder a 202/5xx response already uses, since this is
         * the same "come back shortly" situation) rather than waiting a full
         * refresh_rate.
         */
        trmnl_refresh_apply_fast_poll();
        return;
    }

#if CONFIG_TRMNL_LOG_TEST_ON_BOOT
    /* Debug only - see the Kconfig help. Wi-Fi and registration are both
     * confirmed good by this point, so submit_log()'s guard always passes
     * here; this exercises the real path against the real server on demand
     * rather than waiting for (or faking) an actual failure. */
    submit_log("Test log", "TRMNL_LOG_TEST_ON_BOOT is enabled - turn it off once this is confirmed",
               __FILE__, __LINE__);
#endif

    /* ---- /api/display --------------------------------------------------- */

    ESP_LOGI(TAG, "--- requesting a frame ---");

    /*
     * A double-click reports the button's configured action to the server.
     * The wire format is just a "special_function: true" header
     * (trmnl_build_display_headers()) - the server already knows which action
     * that is, because it is the one it configured and handed back to us in
     * the first place. Reporting the gesture is the device's whole job for
     * six of the seven actions.
     *
     * add_wifi is the exception: it needs a device-local capability the
     * header cannot provide, so it launches the portal directly instead of
     * being reported. Existing credentials are left alone until - and
     * unless - the portal actually collects a replacement, so a user who
     * changes their mind or lets it time out just gets the device back on
     * the network it already knew, same as ClearWifi's own timeout.
     *
     * A long press is ClearWifi - handle_clear_wifi() reboots, so it never
     * falls through to the request this switch is building.
     */
    trmnl_special_function_t report_sf      = TRMNL_SF_NONE;
    bool                     report_sf_flag = false;

    switch (trmnl_sleep_button_gesture()) {
    case TRMNL_BUTTON_DOUBLE:
        report_sf = (trmnl_special_function_t)
            trmnl_persist_get_u32(TRMNL_NVS_SPECIAL_FUNCTION, TRMNL_SF_NONE);

        if (report_sf == TRMNL_SF_ADD_WIFI) {
            ESP_LOGI(TAG, "double-click: add_wifi - launching the provisioning portal");
            trmnl_portal_run(&s_board);
            /* Not reached: trmnl_portal_run() always ends in a reboot or a
             * deep sleep, never a normal return. */
        }

        report_sf_flag = (report_sf != TRMNL_SF_NONE);
        ESP_LOGI(TAG, "double-click: reporting special_function \"%s\" to the server",
                 trmnl_special_function_str(report_sf));
        break;
    case TRMNL_BUTTON_LONG:
        handle_clear_wifi();
        /* Not reached: esp_restart() does not return. */
        break;
    case TRMNL_BUTTON_SINGLE:
    case TRMNL_BUTTON_NONE:
    default:
        break;
    }

    /*
     * Best-effort: a read failure leaves battery zeroed (present=false), and
     * the fallback below is the same placeholder this port sent before the
     * gauge was wired up - a plausible value keeps the server from thinking
     * a USB-only bench unit's battery is flat when there simply isn't one.
     */
    trmnl_battery_status_t battery;
    trmnl_battery_read(&battery);
    if (battery.present) {
        ESP_LOGI(TAG, "Battery: %.2f V%s", battery.voltage_v,
                 battery.charging ? " (charging)" : "");
    } else {
        ESP_LOGI(TAG, "no battery detected; sending the USB-only placeholder voltage");
    }

    trmnl_display_inputs_t display_in = {
        .base_url                = TRMNL_BASE_URL,
        .mac                     = s_mac,
        .api_key                 = s_api_key,
        .firmware_version        = TRMNL_FW_VERSION_STRING,
        .model                   = TRMNL_DEVICE_MODEL,
        .update_source           = trmnl_sleep_update_source(),
        .refresh_rate            = trmnl_refresh_seconds(),
        .battery_voltage         = battery.present ? battery.voltage_v : 4.10f,
        .battery_charging        = battery.charging,
        .report_charging         = battery.present,
        .rssi                    = current_rssi(),
        .display_width           = TRMNL_DISPLAY_WIDTH,
        .display_height          = TRMNL_DISPLAY_HEIGHT,
        .image_cached            = s_used_cached_image,
        .prev_wake_time          = s_prev_wake_time,
        .special_function        = report_sf,
        .report_special_function = report_sf_flag,
    };

    trmnl_display_response_t display_out;
    int http_status = 0;

    esp_err_t err = trmnl_api_display(&display_in, &display_out, &http_status);
    if (err != ESP_OK) {
        s_api_attempts++;
        report_failure(s_api_attempts,
                       trmnl_refresh_apply_api_retry(s_api_attempts),
                       "Server unreachable", esp_err_to_name(err));
        return;
    }
    if (display_out.outcome != TRMNL_DISPLAY_OK) {
        s_api_attempts++;
        report_failure(s_api_attempts,
                       trmnl_refresh_apply_api_retry(s_api_attempts),
                       "Bad response", display_out.error_detail);
        return;
    }
    s_api_attempts = 0;

    ESP_LOGI(TAG, "status           : %d", display_out.status);
    ESP_LOGI(TAG, "filename         : %s", display_out.filename);
    ESP_LOGI(TAG, "refresh_rate     : %" PRIu32 " s", display_out.refresh_rate);
    ESP_LOGI(TAG, "temp_profile     : %" PRIu32, display_out.temp_profile);
    ESP_LOGI(TAG, "special_function : %s",
             trmnl_special_function_str(display_out.special_function));
    ESP_LOGI(TAG, "update_firmware  : %s", display_out.update_firmware ? "yes" : "no");
    ESP_LOGI(TAG, "reset_firmware   : %s", display_out.reset_firmware ? "yes" : "no");

    /*
     * Fast poll. 202 means the plugin has not rendered yet - a device that is
     * registered but not yet linked to a trmnl.app account is indistinguishable
     * from this on the wire - and a 5xx means the server is having a bad
     * minute; upstream answers both by coming back quickly at first and then
     * backing away, rather than either hammering the server or waiting a
     * quarter of an hour for content that is seconds (or one web form) away.
     *
     * The signal can arrive two ways and both have to be checked: a real
     * transport-level HTTP 202/5xx, or - as seen on trmnl.app today - a plain
     * HTTP 200 wrapping a body whose own "status" field says 202. Checking
     * only http_status missed the second form entirely, which is indistinguishable
     * from "nothing to draw" once it reaches update_image() - exactly the
     * "Nothing to show yet" screen a device waiting on account linkage should
     * never actually have to show.
     */
    if (http_status == 202 || http_status >= 500 ||
        display_out.status == 202 || display_out.status >= 500) {
        const uint32_t next = trmnl_refresh_apply_fast_poll();
        ESP_LOGI(TAG, "server is not ready (HTTP %d, status %d); polling again in %" PRIu32 " s",
                 http_status, display_out.status, next);
        return;
    }
    trmnl_refresh_reset_fast_poll_streak();

    /* Persisted, not executed - see remember_special_function(). */
    remember_special_function(display_out.special_function);

    if (display_out.reset_firmware) {
        handle_reset_firmware();
        /* Not reached: esp_restart() does not return. */
    }

    /* TODO(phase 7): OTA via esp_https_ota; the partition table already
     * allows it. */
    if (display_out.update_firmware) {
        ESP_LOGW(TAG, "server asked for a firmware update; not implemented yet");
    }

    /* ---- draw ----------------------------------------------------------- */

    err = update_image(&display_out);
    if (err != ESP_OK) {
        s_api_attempts++;
        const uint32_t next_s = trmnl_refresh_apply_api_retry(s_api_attempts);
        if (err == ESP_ERR_NOT_FOUND) {
            /* Not a fault: the server had nothing to send, most likely
             * because no plugin is assigned to this device yet. Same retry
             * ladder as any other API hiccup, but it shouldn't read like one. */
            report_failure(s_api_attempts, next_s,
                           "Nothing to show yet", "Assign a plugin at trmnl.app");
        } else {
            report_failure(s_api_attempts, next_s, "Image failed", esp_err_to_name(err));
        }
        return;
    }

    /* ---- interval ------------------------------------------------------- */

    if (display_out.refresh_rate != 0) {
        trmnl_refresh_apply_server_rate(display_out.refresh_rate);
    } else {
        /* A response with no usable rate is not an error; fall back rather than
         * store a zero and spin. */
        trmnl_refresh_apply_default();
    }

    if (trmnl_time_is_valid()) {
        s_prev_wake_time = (uint32_t)time(NULL);
    }
}

/* ===========================================================================
 * Main
 * ========================================================================= */

void app_main(void)
{
    /*
     * "Reported" because this is what goes in the Width/Height headers, not
     * necessarily the physical panel - see the TRMNL_PANEL Kconfig choice.
     * trmnl_display_init() logs the actual panel and its real resolution
     * separately, once board init brings it up.
     */
    ESP_LOGI(TAG, "TRMNL firmware %s, model \"%s\", reported panel %dx%d",
             TRMNL_FW_VERSION_STRING, TRMNL_DEVICE_MODEL,
             TRMNL_DISPLAY_WIDTH, TRMNL_DISPLAY_HEIGHT);

    /* Which of the two wait modes this image was built with. Worth a line: the
     * difference is invisible until the button stops working. */
    ESP_LOGI(TAG, "between cycles: %s", trmnl_sleep_is_deep()
             ? "deep sleep, timer wake only (the button does nothing)"
             : "idle, button polled");

    /* ---- Board ---------------------------------------------------------- */

    /*
     * One board init, here, so that the expander handle the SD card's power
     * gate needs and the RTC handle the clock needs both come from the same
     * place. Everything else is switched off except the fuel gauge: the EPD
     * pins are claimed by the epd component rather than the BSP, and the
     * accelerometer is unused.
     *
     * use_sdcard stays false on purpose. The BSP treats the card as a REQUIRED
     * device and fails board init without one; the cache mounts it itself and
     * treats absence as "run uncached". See trmnl_cache.h.
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

    /* The gauge is already running once board init returns (bsp_stc3115_init()
     * handles the STC3115's own start-up sequence) - this just remembers the
     * handle. Marked REQUIRED on this board's schematic, same as the panel
     * PMIC, so a failure here already aborted above rather than needing its
     * own fallback. */
    trmnl_battery_init(s_board.stc3115);

    /*
     * The panel next, matching upstream's Init -> DisplayInit ordering. This
     * only creates the panel and allocates the framebuffer - the rails stay
     * down and nothing is drawn until there is something worth showing, so it
     * costs no refresh time and leaves whatever frame is already on the glass
     * in place. Bringing it up before Wi-Fi means the error paths below have
     * somewhere to report to.
     */
    err = trmnl_display_init(s_board.tps65185);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s - continuing without a panel",
                 esp_err_to_name(err));
    }

    /* ---- Storage and clock ---------------------------------------------- */

    ESP_ERROR_CHECK(trmnl_persist_init());

    /* ---- Wi-Fi provisioning ----------------------------------------------
     *
     * Ahead of the clock and the cache: neither is up yet, so a portal
     * timeout has nothing to tear down before it sleeps (see
     * trmnl_portal_run()'s own doc comment). No NVS entry - a fresh device,
     * or one just cleared via ClearWifi - means no compiled-in fallback
     * either: straight into the portal.
     */
    if (!trmnl_persist_exists(TRMNL_NVS_WIFI_SSID)) {
        const esp_err_t portal_err = trmnl_portal_run(&s_board);
        if (portal_err != ESP_OK) {
            ESP_LOGE(TAG, "provisioning portal failed to start: %s",
                     esp_err_to_name(portal_err));
            /* Falls through: bsp_wifi_connect() below will fail on an empty
             * SSID and land on the existing Wi-Fi retry ladder. */
        }
    }

    /* Seed the clock from the RTC before the network exists, so the cache's
     * timestamps are right even on a cycle where SNTP fails. */
    trmnl_time_init(s_board.rv3028);

    /* A missing card is not an error; the return value is deliberately ignored
     * and every cache call answers "no" from then on. */
    trmnl_cache_init(&s_board_cfg, s_board.tca6408);

    /* ---- Wi-Fi ---------------------------------------------------------- */

    /* bsp_wifi_init() brings up NVS, the default event loop and the esp-hosted
     * link to the C6. Expect roughly 1.5 s here for the radio to reset and
     * re-negotiate SDIO. */
    ESP_ERROR_CHECK(bsp_wifi_init());
    ESP_ERROR_CHECK(device_mac_string(s_mac, sizeof(s_mac)));

    /* ---- The cycle ------------------------------------------------------ */

    /*
     * One iteration per boot in a deep-sleep build - trmnl_sleep_wait() does not
     * return there - and one per refresh_rate when idling.
     */
    for (;;) {
        run_cycle();
        trmnl_sleep_wait(trmnl_refresh_seconds(), &s_board);
    }
}
