#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "trmnl_api.h"
#include "trmnl_config.h"

static const char *TAG = "trmnl_api";

/*
 * Response bodies are small - the largest field is a presigned URL - but the
 * server is free to add fields, so leave headroom over the fields we read.
 */
#define TRMNL_JSON_BODY_MAX 8192

/* ── Special functions ────────────────────────────────────────────────────── */

static const struct {
    const char              *name;
    trmnl_special_function_t value;
} s_special_functions[] = {
    { "none",             TRMNL_SF_NONE             },
    { "identify",         TRMNL_SF_IDENTIFY         },
    { "sleep",            TRMNL_SF_SLEEP            },
    { "add_wifi",         TRMNL_SF_ADD_WIFI         },
    { "restart_playlist", TRMNL_SF_RESTART_PLAYLIST },
    { "rewind",           TRMNL_SF_REWIND           },
    { "send_to_me",       TRMNL_SF_SEND_TO_ME       },
    { "guest_mode",       TRMNL_SF_GUEST_MODE       },
};

trmnl_special_function_t trmnl_special_function_parse(const char *str)
{
    if (!str) {
        return TRMNL_SF_NONE;
    }
    for (size_t i = 0; i < sizeof(s_special_functions) / sizeof(s_special_functions[0]); i++) {
        if (strcmp(str, s_special_functions[i].name) == 0) {
            return s_special_functions[i].value;
        }
    }
    return TRMNL_SF_NONE;
}

const char *trmnl_special_function_str(trmnl_special_function_t sf)
{
    for (size_t i = 0; i < sizeof(s_special_functions) / sizeof(s_special_functions[0]); i++) {
        if (s_special_functions[i].value == sf) {
            return s_special_functions[i].name;
        }
    }
    return "none";
}

/* ── Header building ──────────────────────────────────────────────────────── */

void trmnl_headers_reset(trmnl_headers_t *h)
{
    if (h) {
        memset(h, 0, sizeof(*h));
    }
}

void trmnl_headers_add(trmnl_headers_t *h, const char *name, const char *value)
{
    if (!h || !name || !value) {
        return;
    }

    size_t len = strlen(value) + 1;
    if (h->count >= TRMNL_HEADERS_MAX || h->used + len > sizeof(h->storage)) {
        h->overflow = true;
        ESP_LOGE(TAG, "header list full, dropping %s", name);
        return;
    }

    char *slot = h->storage + h->used;
    memcpy(slot, value, len);
    h->used += len;

    h->items[h->count].name  = name;   /* always a string literal */
    h->items[h->count].value = slot;
    h->count++;
}

void trmnl_headers_addf(trmnl_headers_t *h, const char *name, const char *fmt, ...)
{
    char buf[96];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    trmnl_headers_add(h, name, buf);
}

void trmnl_headers_log(const trmnl_headers_t *h, const char *tag)
{
    if (!h) {
        return;
    }
    for (size_t i = 0; i < h->count; i++) {
        bool secret = strcmp(h->items[i].name, "Access-Token") == 0;
        ESP_LOGI(tag, "  %s: %s", h->items[i].name,
                 secret ? "<redacted>" : h->items[i].value);
    }
}

/* ── Small cJSON helpers ──────────────────────────────────────────────────── */

/* Copy a string field, or leave `dst` as the empty string. Mirrors the
 * `doc["x"] | ""` idiom upstream uses everywhere. */
static void json_str(const cJSON *root, const char *key, char *dst, size_t dst_len)
{
    dst[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

static uint32_t json_u32(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        return (uint32_t)item->valuedouble;
    }
    return 0;
}

static int json_int(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : 0;
}

/* ArduinoJson coerces a missing or non-boolean field to false, so do the same. */
static bool json_bool(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valuedouble != 0;
    }
    return false;
}

/* ── /api/setup ───────────────────────────────────────────────────────────── */

void trmnl_build_setup_headers(const trmnl_setup_inputs_t *in, trmnl_headers_t *out)
{
    trmnl_headers_reset(out);
    trmnl_headers_add(out, "ID",           in->mac);
    trmnl_headers_add(out, "Content-Type", "application/json");
    trmnl_headers_add(out, "FW-Version",   in->firmware_version);
    trmnl_headers_add(out, "Model",        in->model);
}

void trmnl_parse_setup_response(const char *json, trmnl_setup_response_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) {
        ESP_LOGE(TAG, "setup: JSON deserialization error");
        out->outcome = TRMNL_SETUP_DESERIALIZATION_ERROR;
        return;
    }

    out->status = json_int(root, "status");
    json_str(root, "message", out->message, sizeof(out->message));

    /*
     * Upstream returns early here without reading api_key / friendly_id: a
     * non-200 body carries a message and nothing else worth keeping.
     */
    if (out->status != 200) {
        ESP_LOGW(TAG, "setup: status %d (%s)", out->status, out->message);
        out->outcome = TRMNL_SETUP_STATUS_ERROR;
        cJSON_Delete(root);
        return;
    }

    out->outcome = TRMNL_SETUP_OK;
    json_str(root, "api_key",     out->api_key,     sizeof(out->api_key));
    json_str(root, "friendly_id", out->friendly_id, sizeof(out->friendly_id));
    json_str(root, "image_url",   out->image_url,   sizeof(out->image_url));

    cJSON_Delete(root);
}

/* ── /api/display ─────────────────────────────────────────────────────────── */

void trmnl_build_display_headers(const trmnl_display_inputs_t *in, trmnl_headers_t *out)
{
    trmnl_headers_reset(out);

    trmnl_headers_add(out,  "ID",              in->mac);
    trmnl_headers_add(out,  "Content-Type",    "application/json");
    trmnl_headers_add(out,  "Update-Source",   in->update_source ? in->update_source : "unknown");
    trmnl_headers_add(out,  "Access-Token",    in->api_key);
    trmnl_headers_addf(out, "Refresh-Rate",    "%" PRIu32, in->refresh_rate);

    /* Arduino's String(float) renders two decimal places; match it exactly so
     * the server sees the same value shape it does from upstream devices. */
    trmnl_headers_addf(out, "Battery-Voltage", "%.2f", in->battery_voltage);

    if (in->report_charging) {
        trmnl_headers_add(out, "Battery-Charging", in->battery_charging ? "1" : "0");
    }
    if (in->report_usb) {
        trmnl_headers_add(out, "USB-Connected", in->usb_connected ? "true" : "false");
    }

    trmnl_headers_add(out,  "FW-Version",   in->firmware_version);
    trmnl_headers_add(out,  "Model",        in->model);
    trmnl_headers_add(out,  "Image-Cached", in->image_cached ? "true" : "false");
    trmnl_headers_addf(out, "Wake-Time",    "%" PRIu32, in->prev_wake_time);
    trmnl_headers_addf(out, "RSSI",         "%d", in->rssi);

    /* The C6 is 2.4 GHz only, so WiFi-Band is constant. Upstream omits the
     * header entirely when it does not know, which is also acceptable; sending
     * it is more informative and costs nothing. */
    trmnl_headers_add(out, "WiFi-Band", "2.4");

    trmnl_headers_add(out,  "Temperature-Profile", "true");
    trmnl_headers_addf(out, "Width",  "%u", in->display_width);
    trmnl_headers_addf(out, "Height", "%u", in->display_height);

    if (in->report_special_function && in->special_function != TRMNL_SF_NONE) {
        trmnl_headers_add(out, "special_function", "true");
    }
}

void trmnl_parse_display_response(const char *json, trmnl_display_response_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "display: JSON deserialization error");
        out->outcome = TRMNL_DISPLAY_DESERIALIZATION_ERROR;
        if (err) {
            strlcpy(out->error_detail, err, sizeof(out->error_detail));
        }
        return;
    }

    out->outcome           = TRMNL_DISPLAY_OK;
    out->status            = json_int(root, "status");
    out->image_url_timeout = json_u32(root, "image_url_timeout");
    out->refresh_rate      = json_u32(root, "refresh_rate");
    out->update_firmware   = json_bool(root, "update_firmware");
    out->reset_firmware    = json_bool(root, "reset_firmware");
    /* Absent on servers talking to firmware <= 1.6.2; upstream defaults false. */
    out->maximum_compatibility = json_bool(root, "maximum_compatibility");

    json_str(root, "image_url",    out->image_url,    sizeof(out->image_url));
    json_str(root, "filename",     out->filename,     sizeof(out->filename));
    json_str(root, "firmware_url", out->firmware_url, sizeof(out->firmware_url));
    json_str(root, "action",       out->action,       sizeof(out->action));

    char sf[32];
    json_str(root, "special_function", sf, sizeof(sf));
    out->special_function = trmnl_special_function_parse(sf);

    /*
     * "temperature_profile" arrives as a name, not a number. Upstream maps
     * "a" -> 1 and "b" -> 2 and leaves everything else (including "c", which is
     * commented out there) at the default 0.
     */
    char tp[16];
    json_str(root, "temperature_profile", tp, sizeof(tp));
    if (strcmp(tp, "a") == 0) {
        out->temp_profile = TRMNL_TEMP_PROFILE_A;
    } else if (strcmp(tp, "b") == 0) {
        out->temp_profile = TRMNL_TEMP_PROFILE_B;
    } else {
        out->temp_profile = TRMNL_TEMP_PROFILE_DEFAULT;
    }

    cJSON_Delete(root);
}

/* ── Whole-call convenience wrappers ──────────────────────────────────────── */

esp_err_t trmnl_api_setup(const trmnl_setup_inputs_t *in,
                           trmnl_setup_response_t     *out,
                           int                        *http_status)
{
    char url[TRMNL_URL_MAX];
    snprintf(url, sizeof(url), "%s%s", in->base_url, TRMNL_API_SETUP_PATH);

    trmnl_headers_t headers;
    trmnl_build_setup_headers(in, &headers);

    ESP_LOGI(TAG, "GET %s", url);
    trmnl_headers_log(&headers, TAG);

    trmnl_http_response_t resp;
    esp_err_t err = trmnl_http_get(url, headers.items, headers.count,
                                   TRMNL_JSON_BODY_MAX, 15000, &resp);
    if (http_status) {
        *http_status = resp.status;
    }
    if (err != ESP_OK) {
        trmnl_http_response_free(&resp);
        memset(out, 0, sizeof(*out));
        out->outcome = TRMNL_SETUP_DESERIALIZATION_ERROR;
        return err;
    }

    ESP_LOGI(TAG, "HTTP %d, %u bytes: %s", resp.status, (unsigned)resp.body_len,
             resp.body ? (const char *)resp.body : "");

    trmnl_parse_setup_response(resp.body ? (const char *)resp.body : NULL, out);
    trmnl_http_response_free(&resp);
    return ESP_OK;
}

esp_err_t trmnl_api_display(const trmnl_display_inputs_t *in,
                             trmnl_display_response_t     *out,
                             int                          *http_status)
{
    char url[TRMNL_URL_MAX];
    snprintf(url, sizeof(url), "%s%s", in->base_url, TRMNL_API_DISPLAY_PATH);

    trmnl_headers_t headers;
    trmnl_build_display_headers(in, &headers);

    ESP_LOGI(TAG, "GET %s", url);
    trmnl_headers_log(&headers, TAG);

    trmnl_http_response_t resp;
    esp_err_t err = trmnl_http_get(url, headers.items, headers.count,
                                   TRMNL_JSON_BODY_MAX, 15000, &resp);
    if (http_status) {
        *http_status = resp.status;
    }
    if (err != ESP_OK) {
        trmnl_http_response_free(&resp);
        memset(out, 0, sizeof(*out));
        out->outcome = TRMNL_DISPLAY_DESERIALIZATION_ERROR;
        return err;
    }

    ESP_LOGI(TAG, "HTTP %d, %u bytes: %s", resp.status, (unsigned)resp.body_len,
             resp.body ? (const char *)resp.body : "");

    trmnl_parse_display_response(resp.body ? (const char *)resp.body : NULL, out);
    trmnl_http_response_free(&resp);
    return ESP_OK;
}

/* ── /api/log ─────────────────────────────────────────────────────────────── */

void trmnl_build_log_headers(const trmnl_log_inputs_t *in, trmnl_headers_t *out)
{
    trmnl_headers_reset(out);
    trmnl_headers_add(out, "ID",           in->mac);
    trmnl_headers_add(out, "Accept",       "application/json, */*");
    trmnl_headers_add(out, "Access-Token", in->api_key);
    trmnl_headers_add(out, "Content-Type", "application/json");
}

static const char *log_level_str(trmnl_log_level_t level)
{
    switch (level) {
    case TRMNL_LOG_DEBUG: return "debug";
    case TRMNL_LOG_WARN:  return "warn";
    case TRMNL_LOG_ERROR: return "error";
    case TRMNL_LOG_FATAL: return "fatal";
    case TRMNL_LOG_INFO:
    default:              return "info";
    }
}

/*
 * cJSON's number field is a double, and widening a float straight into one
 * carries its binary imprecision along - 4.2f is exactly 4.19999980926513671875
 * once it's a double, and cJSON's printer (correctly) shows that rather than
 * hiding it. Round-tripping through a fixed-precision string first, the way
 * the Battery-Voltage *header* already does elsewhere in this file, lands on
 * the double actually closest to the decimal value instead.
 */
static double round2f(float v)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", (double)v);
    return strtod(buf, NULL);
}

char *trmnl_serialize_log_entry(const trmnl_log_entry_t *e)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    /*
     * Field order matches serialize_log.cpp's own assignment order exactly.
     * That's cosmetic once the server parses it - JSON key order isn't
     * meaningful - but it's one less thing to think about when comparing
     * this output against upstream's test fixture by eye.
     */
    cJSON_AddNumberToObject(root, "created_at", (double)e->timestamp);
    cJSON_AddNumberToObject(root, "id", e->log_id);
    cJSON_AddStringToObject(root, "message", e->message ? e->message : "");
    cJSON_AddNumberToObject(root, "source_line", e->source_line);
    cJSON_AddStringToObject(root, "source_path", e->source_file ? e->source_file : "");
    cJSON_AddNumberToObject(root, "wifi_signal", e->wifi_rssi_level);
    cJSON_AddStringToObject(root, "wifi_status", e->wifi_status ? e->wifi_status : "");
    cJSON_AddNumberToObject(root, "refresh_rate", e->refresh_rate);
    cJSON_AddNumberToObject(root, "sleep_duration", e->sleep_duration);
    cJSON_AddStringToObject(root, "firmware_version", e->firmware_version ? e->firmware_version : "");
    cJSON_AddStringToObject(root, "special_function", e->special_function ? e->special_function : "");
    cJSON_AddNumberToObject(root, "battery_voltage", round2f(e->battery_voltage));
    cJSON_AddStringToObject(root, "wake_reason", e->wake_reason ? e->wake_reason : "");
    cJSON_AddNumberToObject(root, "free_heap_size", e->free_heap_size);
    cJSON_AddNumberToObject(root, "max_alloc_size", e->max_alloc_size);
    cJSON_AddStringToObject(root, "level", log_level_str(e->level));
    if (e->has_retry) {
        cJSON_AddNumberToObject(root, "retry", e->retry_attempt);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

esp_err_t trmnl_api_log(const trmnl_log_inputs_t *in,
                         const trmnl_log_entry_t  *entry,
                         int                       *http_status)
{
    char *entry_json = trmnl_serialize_log_entry(entry);
    if (!entry_json) {
        return ESP_ERR_NO_MEM;
    }

    /* {"logs":[<entry>]} - upstream's serializeApiLogRequest(), one entry at
     * a time; see this function's doc comment for why. */
    size_t body_cap = strlen(entry_json) + 16;
    char *body = malloc(body_cap);
    if (!body) {
        cJSON_free(entry_json);
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_cap, "{\"logs\":[%s]}", entry_json);
    cJSON_free(entry_json);

    char url[TRMNL_URL_MAX];
    snprintf(url, sizeof(url), "%s%s", in->base_url, TRMNL_API_LOG_PATH);

    trmnl_headers_t headers;
    trmnl_build_log_headers(in, &headers);

    ESP_LOGI(TAG, "POST %s", url);
    trmnl_headers_log(&headers, TAG);
    ESP_LOGI(TAG, "  body: %s", body);

    trmnl_http_response_t resp;
    esp_err_t err = trmnl_http_post(url, headers.items, headers.count,
                                    body, strlen(body),
                                    TRMNL_JSON_BODY_MAX, 15000, &resp);
    free(body);

    if (http_status) {
        *http_status = resp.status;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP %d", resp.status);
    }
    trmnl_http_response_free(&resp);
    return err;
}
