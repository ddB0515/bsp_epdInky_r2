#include "ha_portal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "tps65185.h"

#include "ha_config.h"
#include "ha_display.h"
#include "ha_dns.h"
#include "ha_persist.h"

/* Pre-fill the stage-2 form's MQTT-port and refresh-interval fields with the
 * build-time Kconfig defaults, so a user who has no reason to change them
 * only has to fill in the fields that matter to their own setup. */
#define HA_PORTAL_MQTT_PORT_DEFAULT_STR HA_STR(CONFIG_HA_MQTT_DEFAULT_PORT)
#define HA_PORTAL_REFRESH_S_DEFAULT_STR HA_STR(CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S)

#include "ha_portal_config_page.h"
#include "ha_portal_wifi_page.h"

static const char *TAG = "ha_portal";

/* An arbitrary-but-common 2.4 GHz mid-band channel. */
#define HA_PORTAL_WIFI_CHANNEL 6

/* "Prevent dead batteries": don't hold either stage's server up indefinitely
 * if nobody is provisioning. Reused for the retry sleep too - there's no
 * signal calling for a different number. */
#define HA_PORTAL_TIMEOUT_MS     (15 * 60 * 1000)
#define HA_PORTAL_RETRY_SLEEP_S  (15 * 60)

/* Generous but still bounded: the stage-2 form carries several long fields (a
 * dashboard URL, a Home Assistant token) that TRMNL's portal never needed
 * to, so its own 512-byte cap would not fit a realistic submission. */
#define HA_PORTAL_BODY_MAX 2048
#define HA_PORTAL_SCAN_MAX 16

static char s_redirect_url[32];

/* ===========================================================================
 * Small helpers, shared by both stages
 * ========================================================================= */

/** Copy a JSON field's value (string or number) into @p out, bounded by
 *  @p out_len. Leaves @p out untouched (caller must have already set a
 *  default) if the field is absent or the wrong type. */
static void copy_json_str(const cJSON *root, const char *field, char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(out, item->valuestring, out_len);
    }
}

/** Same, but leaves @p out untouched on an *empty* string too, not just an
 *  absent field. For the two secret fields (mqtt_pass, ha_token) the stage-2
 *  page never echoes the real stored value back to the browser to prefill
 *  it (see handle_config_current()), so a blank submission is ambiguous
 *  between "the user cleared it" and "the user never touched it" - this
 *  resolves that ambiguity in favour of keeping whatever @p out was already
 *  seeded with (the current NVS value), which is the only way to leave a
 *  secret unchanged without ever sending it back to the browser. */
static void copy_json_str_keep_if_blank(const cJSON *root, const char *field,
                                        char *out, size_t out_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        strlcpy(out, item->valuestring, out_len);
    }
}

/** Same, but for a field that arrives as a numeric string (from an <input
 *  type="number">) or a bare JSON number - clamped to [min, max], falling
 *  back to @p fallback if absent, unparsable, or out of range. */
static uint32_t parse_json_uint(const cJSON *root, const char *field,
                                uint32_t fallback, uint32_t min, uint32_t max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    long value;

    if (cJSON_IsNumber(item)) {
        value = (long)item->valuedouble;
    } else if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        char *end = NULL;
        value = strtol(item->valuestring, &end, 10);
        if (end == item->valuestring) {
            return fallback; /* not a number at all */
        }
    } else {
        return fallback;
    }

    if (value < (long)min || value > (long)max) {
        return fallback;
    }
    return (uint32_t)value;
}

static void get_mac_str(char *out, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t recv_body(httpd_req_t *req, char **out_body)
{
    if (req->content_len <= 0 || req->content_len >= HA_PORTAL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char *body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';
    *out_body = body;
    return ESP_OK;
}

/* ===========================================================================
 * Shared teardown: timeout -> stop server -> sleep and retry
 * ========================================================================= */

/** Blocks for HA_PORTAL_TIMEOUT_MS. If nobody has submitted the form by
 *  then (a submit reboots from inside its own handler, so this is never
 *  reached in that case), tears the server down and deep-sleeps regardless
 *  of the configured idle/deep-sleep Kconfig choice - the point is to not
 *  hold a radio and an HTTP server up indefinitely on a battery. The device
 *  re-evaluates which stage it needs on its next wake. */
static void wait_then_sleep_on_timeout(httpd_handle_t server, const bsp_epdinky_handles_t *board,
                                       bool stop_dns)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HA_PORTAL_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "setup timed out after %d minutes with nobody configuring; sleeping",
             HA_PORTAL_TIMEOUT_MS / 60000);

    httpd_stop(server);
    if (stop_dns) {
        ha_dns_stop();
    }
    bsp_wifi_deinit();
    if (board != NULL && board->tps65185 != NULL) {
        tps65185_sleep(board->tps65185);
    }

    esp_sleep_enable_timer_wakeup((uint64_t)HA_PORTAL_RETRY_SLEEP_S * 1000000ULL);
    esp_deep_sleep_start();
    /* Not reached. */
}

/* ===========================================================================
 * Stage 1: Wi-Fi-only SoftAP captive portal
 * ========================================================================= */

static esp_err_t handle_wifi_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HA_PORTAL_WIFI_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    bsp_wifi_ap_t aps[HA_PORTAL_SCAN_MAX];
    size_t        found = 0;

    /* Errors here just mean an empty list - typing the SSID in by hand still
     * works, so there is nothing worth failing the request over. */
    bsp_wifi_scan(aps, HA_PORTAL_SCAN_MAX, &found);

    cJSON *root     = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < found; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue; /* hidden network - nothing to show or click */
        }
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "name", aps[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(n, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, n);
    }

    char mac_str[18];
    get_mac_str(mac_str, sizeof(mac_str));
    cJSON_AddStringToObject(root, "mac", mac_str);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root      = cJSON_Parse(body);
    free(body);
    cJSON *ssid_item = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid is required");
        return ESP_FAIL;
    }

    /* Fixed-size destination buffers, copied through strlcpy so an over-long
     * SSID/password is truncated rather than overflowing anything. */
    char ssid[HA_WIFI_SSID_MAX] = { 0 };
    char pswd[HA_WIFI_PASS_MAX] = { 0 };
    strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));
    copy_json_str(root, "pswd", pswd, sizeof(pswd));
    cJSON_Delete(root);

    ha_persist_set_str(HA_NVS_WIFI_SSID, ssid);
    ha_persist_set_str(HA_NVS_WIFI_PASS, pswd);

    char mac_str[18];
    get_mac_str(mac_str, sizeof(mac_str));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "ssid", ssid);
    cJSON_AddStringToObject(resp, "mac", mac_str);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "Wi-Fi saved (\"%s\"); rebooting to connect and finish setup", ssid);

    /* The delay is so the response above actually reaches the browser before
     * the reboot tears down the socket it's on. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK; /* not reached */
}

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_redirect_url);
    return httpd_resp_send(req, NULL, 0);
}

/* Windows 11's own captive-portal check expects this exact target, not the
 * portal's own address. */
static esp_err_t handle_connecttest(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://logout.net");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_200_empty(httpd_req_t *req)
{
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_404(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
}

static esp_err_t handle_404_err(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return handle_redirect(req);
}

/*
 * The one-line routes below exist for one reason: get every OS's captive-
 * portal probe to land on "/" instead of quietly failing. Kept close to the
 * trmnl-firmware example's own set - these are hard-won per-OS quirks, not
 * padding. Stage 2 (a plain page on a real network) needs none of them.
 */
static const httpd_uri_t s_wifi_get_routes[] = {
    { .uri = "/",                    .method = HTTP_GET, .handler = handle_wifi_root },
    { .uri = "/scan",                .method = HTTP_GET, .handler = handle_wifi_scan },
    { .uri = "/generate_204",        .method = HTTP_GET, .handler = handle_redirect },      /* Android */
    { .uri = "/redirect",            .method = HTTP_GET, .handler = handle_redirect },      /* Microsoft */
    { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_redirect },      /* Apple */
    { .uri = "/canonical.html",      .method = HTTP_GET, .handler = handle_redirect },      /* Firefox */
    { .uri = "/ncsi.txt",            .method = HTTP_GET, .handler = handle_redirect },      /* Windows */
    { .uri = "/connecttest.txt",     .method = HTTP_GET, .handler = handle_connecttest },   /* Windows 11 */
    { .uri = "/success.txt",         .method = HTTP_GET, .handler = handle_200_empty },     /* Firefox */
    { .uri = "/wpad.dat",            .method = HTTP_GET, .handler = handle_404 },           /* stops a Win10 retry storm */
    { .uri = "/favicon.ico",         .method = HTTP_GET, .handler = handle_404 },
};

static const httpd_uri_t s_wifi_post_routes[] = {
    { .uri = "/connect", .method = HTTP_POST, .handler = handle_wifi_connect },
};

esp_err_t ha_portal_run_wifi(const bsp_epdinky_handles_t *board)
{
    esp_err_t err = bsp_wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "ha-epdinky-%02x%02x%02x", mac[3], mac[4], mac[5]);

    err = bsp_wifi_ap_start(ssid, NULL, HA_PORTAL_WIFI_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        return err;
    }

    char ap_ip[16];
    bsp_wifi_ap_get_ip_str(ap_ip, sizeof(ap_ip));
    snprintf(s_redirect_url, sizeof(s_redirect_url), "http://%s/", ap_ip);

    ha_dns_start((uint32_t)inet_addr(ap_ip));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size       = 6144; /* scan results + cJSON on top of the default */
    config.lru_purge_enable = true;

    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        ha_dns_stop();
        return err;
    }
    for (size_t i = 0; i < sizeof(s_wifi_get_routes) / sizeof(s_wifi_get_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_wifi_get_routes[i]);
    }
    for (size_t i = 0; i < sizeof(s_wifi_post_routes) / sizeof(s_wifi_post_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_wifi_post_routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_404_err);

    ha_display_show_wifi_setup(ssid, s_redirect_url, HA_FW_VERSION_STRING);

    ESP_LOGI(TAG, "Wi-Fi portal up: join \"%s\" (open), then visit %s", ssid, s_redirect_url);

    wait_then_sleep_on_timeout(server, board, /* stop_dns = */ true);
    return ESP_OK; /* not reached */
}

/* ===========================================================================
 * Stage 2: MQTT/Home Assistant config server, on the real station network
 * ========================================================================= */

static esp_err_t handle_config_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HA_PORTAL_CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
}

/*
 * What the stage-2 page prefills its form with, so a "reconfigure" doesn't
 * mean retyping everything. mqtt_pass/ha_token are deliberately reported as
 * booleans ("already set?"), never their real values - unlike the rest of
 * this form, a secret already in NVS is not sent back to the browser to be
 * displayed. See copy_json_str_keep_if_blank()'s comment for how a blank
 * resubmission is then interpreted as "leave it as is".
 */
static esp_err_t handle_config_current(httpd_req_t *req)
{
    char mqtt_host[HA_MQTT_HOST_MAX] = { 0 };
    char mqtt_user[HA_MQTT_USER_MAX] = { 0 };
    char mqtt_pass[HA_MQTT_PASS_MAX] = { 0 };
    char image_url[HA_IMAGE_URL_MAX] = { 0 };
    char ha_token[HA_TOKEN_MAX]      = { 0 };

    ha_persist_get_str(HA_NVS_MQTT_HOST, mqtt_host, sizeof(mqtt_host));
    ha_persist_get_str(HA_NVS_MQTT_USER, mqtt_user, sizeof(mqtt_user));
    ha_persist_get_str(HA_NVS_MQTT_PASS, mqtt_pass, sizeof(mqtt_pass));
    ha_persist_get_str(HA_NVS_IMAGE_URL, image_url, sizeof(image_url));
    ha_persist_get_str(HA_NVS_HA_TOKEN, ha_token, sizeof(ha_token));
    const uint32_t mqtt_port = ha_persist_get_u32(HA_NVS_MQTT_PORT, CONFIG_HA_MQTT_DEFAULT_PORT);
    const uint32_t refresh_s = ha_persist_get_u32(HA_NVS_REFRESH_S,
                                                  CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mqtt_host", mqtt_host);
    cJSON_AddNumberToObject(root, "mqtt_port", mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_user", mqtt_user);
    cJSON_AddBoolToObject(root, "mqtt_pass_set", mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(root, "image_url", image_url);
    cJSON_AddBoolToObject(root, "ha_token_set", ha_token[0] != '\0');
    cJSON_AddNumberToObject(root, "refresh_s", refresh_s);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_config_connect(httpd_req_t *req)
{
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root           = cJSON_Parse(body);
    free(body);
    cJSON *mqtt_host_item = root ? cJSON_GetObjectItemCaseSensitive(root, "mqtt_host") : NULL;
    cJSON *image_url_item = root ? cJSON_GetObjectItemCaseSensitive(root, "image_url") : NULL;

    /*
     * The two fields the rest of this firmware cannot run a cycle without:
     * no broker means no MQTT telemetry/discovery, no image URL means
     * nothing to fetch. MQTT username/password, the HA token and the refresh
     * interval are all allowed to be blank/default - see main.c and
     * ha_mqtt.c for how each absence is handled at runtime.
     */
    if (!cJSON_IsString(mqtt_host_item) || mqtt_host_item->valuestring[0] == '\0' ||
        !cJSON_IsString(image_url_item) || image_url_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "mqtt_host and image_url are required");
        return ESP_FAIL;
    }

    /*
     * Every destination buffer below is fixed-size and every copy goes
     * through strlcpy/parse_json_uint, so an over-long or malformed field is
     * truncated or clamped rather than overflowing anything.
     *
     * mqtt_pass and ha_token are seeded from whatever is already in NVS
     * (empty on a first-time save) before the JSON body is applied: the
     * stage-2 page never sends the real secret back to prefill the form
     * (see handle_config_current()), so an untouched, still-blank field in
     * the submission has to mean "keep the current value", not "clear it" -
     * see copy_json_str_keep_if_blank()'s own comment.
     */
    char mqtt_host[HA_MQTT_HOST_MAX] = { 0 };
    char mqtt_user[HA_MQTT_USER_MAX] = { 0 };
    char mqtt_pass[HA_MQTT_PASS_MAX] = { 0 };
    char image_url[HA_IMAGE_URL_MAX] = { 0 };
    char ha_token[HA_TOKEN_MAX]      = { 0 };

    ha_persist_get_str(HA_NVS_MQTT_PASS, mqtt_pass, sizeof(mqtt_pass));
    ha_persist_get_str(HA_NVS_HA_TOKEN, ha_token, sizeof(ha_token));

    strlcpy(mqtt_host, mqtt_host_item->valuestring, sizeof(mqtt_host));
    strlcpy(image_url, image_url_item->valuestring, sizeof(image_url));
    copy_json_str(root, "mqtt_user", mqtt_user, sizeof(mqtt_user));
    copy_json_str_keep_if_blank(root, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass));
    copy_json_str_keep_if_blank(root, "ha_token", ha_token, sizeof(ha_token));

    const uint32_t mqtt_port = parse_json_uint(root, "mqtt_port",
                                               ha_persist_get_u32(HA_NVS_MQTT_PORT,
                                                                  CONFIG_HA_MQTT_DEFAULT_PORT),
                                               1, 65535);
    const uint32_t refresh_s = parse_json_uint(root, "refresh_s",
                                               ha_persist_get_u32(HA_NVS_REFRESH_S,
                                                                  CONFIG_HA_DEFAULT_REFRESH_INTERVAL_S),
                                               HA_REFRESH_S_MIN, HA_REFRESH_S_MAX);
    cJSON_Delete(root);

    ha_persist_set_str(HA_NVS_MQTT_HOST, mqtt_host);
    ha_persist_set_u32(HA_NVS_MQTT_PORT, mqtt_port);
    ha_persist_set_str(HA_NVS_MQTT_USER, mqtt_user);
    ha_persist_set_str(HA_NVS_MQTT_PASS, mqtt_pass);
    ha_persist_set_str(HA_NVS_IMAGE_URL, image_url);
    ha_persist_set_str(HA_NVS_HA_TOKEN, ha_token);
    ha_persist_set_u32(HA_NVS_REFRESH_S, refresh_s);
    ha_persist_erase(HA_NVS_FORCE_CFG); /* settings are complete again */

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "mqtt_host", mqtt_host);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "configuration saved (MQTT %s:%" PRIu32 "); rebooting",
             mqtt_host, mqtt_port);

    /* The delay is so the response above actually reaches the browser before
     * the reboot tears down the socket it's on. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK; /* not reached */
}

static const httpd_uri_t s_config_get_routes[] = {
    { .uri = "/",            .method = HTTP_GET,  .handler = handle_config_root },
    { .uri = "/current",     .method = HTTP_GET,  .handler = handle_config_current },
    { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = handle_404 },
};

static const httpd_uri_t s_config_post_routes[] = {
    { .uri = "/connect", .method = HTTP_POST, .handler = handle_config_connect },
};

esp_err_t ha_portal_run_config(const bsp_epdinky_handles_t *board)
{
    /* Wi-Fi is already up and connected in station mode by the caller - only
     * the HTTP server needs starting here. */
    char ip[16] = "?";
    bsp_wifi_get_ip_str(ip, sizeof(ip));
    snprintf(s_redirect_url, sizeof(s_redirect_url), "http://%s/", ip);

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size       = 6144; /* cJSON on top of the default */
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(s_config_get_routes) / sizeof(s_config_get_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_config_get_routes[i]);
    }
    for (size_t i = 0; i < sizeof(s_config_post_routes) / sizeof(s_config_post_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_config_post_routes[i]);
    }

    ha_display_message("Wi-Fi connected", s_redirect_url,
                       "Open this address to finish MQTT + Home Assistant setup");

    ESP_LOGI(TAG, "config server up at %s - waiting for MQTT/dashboard setup", s_redirect_url);

    wait_then_sleep_on_timeout(server, board, /* stop_dns = */ false);
    return ESP_OK; /* not reached */
}
