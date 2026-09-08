/*
 * TRMNL server protocol: request headers and response parsing.
 *
 * Ported from upstream `lib/trmnl/` (api_types.h, api-client/request_headers.cpp,
 * parse_response_api_setup.cpp, parse_response_api_display.cpp,
 * special_function.cpp), rewritten in C against cJSON. The wire format - header
 * names, field names, defaults, the temperature-profile mapping - is kept
 * byte-identical to upstream so the server sees a device it recognises.
 *
 * Strings are fixed-size arrays rather than heap pointers. The response fields
 * have known bounds (a friendly ID is six characters, a presigned URL a few
 * hundred) and this keeps the whole result copyable and free-able as one unit.
 */

#ifndef TRMNL_API_H
#define TRMNL_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#include "trmnl_http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRMNL_URL_MAX          512
#define TRMNL_API_KEY_MAX       64
#define TRMNL_FRIENDLY_ID_MAX   32
#define TRMNL_FILENAME_MAX     128
#define TRMNL_MESSAGE_MAX      192

/* ── Special functions ────────────────────────────────────────────────────── */

typedef enum {
    TRMNL_SF_NONE = 0,
    TRMNL_SF_IDENTIFY,
    TRMNL_SF_SLEEP,
    TRMNL_SF_ADD_WIFI,
    TRMNL_SF_RESTART_PLAYLIST,
    TRMNL_SF_REWIND,
    TRMNL_SF_SEND_TO_ME,
    TRMNL_SF_GUEST_MODE,
} trmnl_special_function_t;

/** @brief Map a wire string ("identify", "sleep", ...) to the enum. */
trmnl_special_function_t trmnl_special_function_parse(const char *str);

/** @brief Wire string for a special function. Never NULL. */
const char *trmnl_special_function_str(trmnl_special_function_t sf);

/* ── Header building ──────────────────────────────────────────────────────── */

#define TRMNL_HEADERS_MAX       24
#define TRMNL_HEADER_STORAGE  1024

/**
 * Header list with its own backing store.
 *
 * Numeric headers (RSSI, Battery-Voltage, Width, ...) have to be formatted
 * somewhere, and pointing trmnl_http_header_t at a stack buffer that has gone
 * out of scope is an easy mistake to make. Values are copied in here instead,
 * so the list stays valid for as long as the struct does.
 */
typedef struct {
    trmnl_http_header_t items[TRMNL_HEADERS_MAX];
    size_t              count;
    char                storage[TRMNL_HEADER_STORAGE];
    size_t              used;
    bool                overflow;  /**< true when something did not fit */
} trmnl_headers_t;

void trmnl_headers_reset(trmnl_headers_t *h);
void trmnl_headers_add(trmnl_headers_t *h, const char *name, const char *value);
void trmnl_headers_addf(trmnl_headers_t *h, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** @brief Log the header list at INFO, with Access-Token redacted. */
void trmnl_headers_log(const trmnl_headers_t *h, const char *tag);

/* ── /api/setup ───────────────────────────────────────────────────────────── */

typedef enum {
    TRMNL_SETUP_OK = 0,
    TRMNL_SETUP_DESERIALIZATION_ERROR,
    TRMNL_SETUP_STATUS_ERROR,
} trmnl_setup_outcome_t;

typedef struct {
    trmnl_setup_outcome_t outcome;
    int                   status;
    char                  api_key[TRMNL_API_KEY_MAX];
    char                  friendly_id[TRMNL_FRIENDLY_ID_MAX];
    char                  image_url[TRMNL_URL_MAX];
    char                  message[TRMNL_MESSAGE_MAX];
} trmnl_setup_response_t;

typedef struct {
    const char *base_url;          /**< e.g. "https://trmnl.app"             */
    const char *mac;               /**< "AA:BB:CC:DD:EE:FF", uppercase       */
    const char *firmware_version;  /**< e.g. "1.8.16"                        */
    const char *model;             /**< e.g. "x"                             */
} trmnl_setup_inputs_t;

/** @brief Build the /api/setup request headers. */
void trmnl_build_setup_headers(const trmnl_setup_inputs_t *in, trmnl_headers_t *out);

/** @brief Parse an /api/setup response body. Never fails; check `outcome`. */
void trmnl_parse_setup_response(const char *json, trmnl_setup_response_t *out);

/* ── /api/display ─────────────────────────────────────────────────────────── */

typedef enum {
    TRMNL_DISPLAY_OK = 0,
    TRMNL_DISPLAY_DESERIALIZATION_ERROR,
} trmnl_display_outcome_t;

typedef struct {
    trmnl_display_outcome_t outcome;
    int                     status;
    char                    image_url[TRMNL_URL_MAX];
    uint32_t                image_url_timeout;
    char                    filename[TRMNL_FILENAME_MAX];
    bool                    update_firmware;
    bool                    maximum_compatibility;
    char                    firmware_url[TRMNL_URL_MAX];
    uint32_t                refresh_rate;
    uint32_t                temp_profile;   /**< "default"/"a"/"b" -> 0/1/2  */
    bool                    reset_firmware;
    trmnl_special_function_t special_function;
    char                    action[32];
    char                    error_detail[96];
} trmnl_display_response_t;

typedef struct {
    const char *base_url;
    const char *mac;
    const char *api_key;
    const char *firmware_version;
    const char *model;
    const char *update_source;     /**< wake reason: "timer", "button", ...   */
    uint32_t    refresh_rate;
    float       battery_voltage;
    bool        battery_charging;
    bool        report_charging;   /**< send the Battery-Charging header      */
    bool        usb_connected;
    bool        report_usb;        /**< send the USB-Connected header         */
    int         rssi;
    uint16_t    display_width;
    uint16_t    display_height;
    bool        image_cached;
    uint32_t    prev_wake_time;    /**< epoch seconds, 0 to omit              */
    trmnl_special_function_t special_function;
    bool        report_special_function;
} trmnl_display_inputs_t;

/** @brief Build the /api/display request headers. */
void trmnl_build_display_headers(const trmnl_display_inputs_t *in, trmnl_headers_t *out);

/** @brief Parse an /api/display response body. Never fails; check `outcome`. */
void trmnl_parse_display_response(const char *json, trmnl_display_response_t *out);

/* ── Convenience: perform the whole call ──────────────────────────────────── */

/**
 * @brief  GET {base_url}/api/setup and parse the reply.
 *
 * @param http_status  Receives the HTTP status code, or NULL.
 */
esp_err_t trmnl_api_setup(const trmnl_setup_inputs_t *in,
                           trmnl_setup_response_t     *out,
                           int                        *http_status);

/**
 * @brief  GET {base_url}/api/display and parse the reply.
 */
esp_err_t trmnl_api_display(const trmnl_display_inputs_t *in,
                             trmnl_display_response_t     *out,
                             int                          *http_status);

/* ── /api/log ─────────────────────────────────────────────────────────────── */

/** Upstream's LogLevel (trmnl_log.h) - the wire strings are "debug".."fatal",
 *  produced by trmnl_serialize_log_entry(), not these names directly. */
typedef enum {
    TRMNL_LOG_DEBUG = 0,
    TRMNL_LOG_INFO,
    TRMNL_LOG_WARN,
    TRMNL_LOG_ERROR,
    TRMNL_LOG_FATAL,
} trmnl_log_level_t;

/**
 * One log entry - upstream's LogWithDetails plus the DeviceStatusStamp it
 * embeds, flattened into a single struct (C has no nested designated
 * initializer worth a second type here). Field names intentionally don't
 * match the JSON keys they produce one-for-one; see
 * trmnl_serialize_log_entry().
 */
typedef struct {
    /* device status stamp */
    int8_t      wifi_rssi_level;
    const char *wifi_status;       /**< e.g. "Connected" / "Disconnected"     */
    uint32_t    refresh_rate;
    uint32_t    sleep_duration;
    const char *firmware_version;
    const char *special_function;  /**< the wire string, e.g. "identify"      */
    float       battery_voltage;
    const char *wake_reason;       /**< "powercycle" / "timer" / "button"     */
    uint32_t    free_heap_size;
    uint32_t    max_alloc_size;

    /* the entry itself */
    uint32_t          log_id;
    time_t            timestamp;   /**< epoch seconds; 0 if the clock isn't set yet */
    const char       *source_file;
    int               source_line;
    const char       *message;
    trmnl_log_level_t level;
    bool              has_retry;   /**< upstream only sends "retry" when true */
    int               retry_attempt;
} trmnl_log_entry_t;

typedef struct {
    const char *base_url;
    const char *mac;
    const char *api_key;
} trmnl_log_inputs_t;

/** @brief Build the /api/log request headers. */
void trmnl_build_log_headers(const trmnl_log_inputs_t *in, trmnl_headers_t *out);

/**
 * @brief  Serialize one log entry, field-for-field identical to upstream's
 *         serialize_log.cpp (verified against its own test).
 *
 * @return A cJSON_PrintUnformatted() string the caller must free() with
 *         cJSON_free(), or NULL on allocation failure.
 */
char *trmnl_serialize_log_entry(const trmnl_log_entry_t *entry);

/**
 * @brief  POST one log entry to {base_url}/api/log.
 *
 * Upstream batches multiple stored entries into one request body
 * (`serializeApiLogRequest()`, `{"logs":[entry, entry, ...]}`); this port
 * always sends exactly one; see the doc comment on `submit_log()` in
 * `main.c` for why that's the deliberate v1 scope rather than an oversight.
 *
 * @param http_status  Receives the HTTP status code, or NULL.
 */
esp_err_t trmnl_api_log(const trmnl_log_inputs_t *in,
                         const trmnl_log_entry_t  *entry,
                         int                       *http_status);

#ifdef __cplusplus
}
#endif

#endif /* TRMNL_API_H */
