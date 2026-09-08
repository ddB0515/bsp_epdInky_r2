#include "ha_portal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "bsp/epdinky_p4_board.h"

#include "ha_config.h"
#include "ha_dns.h"
#include "ha_persist.h"
#include "ha_setup_screen.h"

/* Pre-fill the stage-2 form's port field with the build-time Kconfig
 * default, so a user who has no reason to change it only has to fill in the
 * fields that matter to their own setup. */
#define HA_PORTAL_WS_PORT_DEFAULT_STR HA_STR(CONFIG_HA_WS_DEFAULT_PORT)

#include "ha_portal_config_page.h"
#include "ha_portal_wifi_page.h"

static const char *TAG = "ha_portal";

/* An arbitrary-but-common 2.4 GHz mid-band channel. */
#define HA_PORTAL_WIFI_CHANNEL 6

#define HA_PORTAL_BODY_MAX 1024
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
 *  absent field. The stage-2 page never echoes the real stored token back to
 *  the browser to prefill it (see handle_config_current()), so a blank
 *  submission is ambiguous between "the user cleared it" and "the user never
 *  touched it" - this resolves that ambiguity in favour of keeping whatever
 *  @p out was already seeded with (the current NVS value), which is the only
 *  way to leave a secret unchanged without ever sending it back to the
 *  browser. */
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

static esp_err_t handle_200_empty(httpd_req_t *req)
{
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_404(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
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

static esp_err_t handle_404_err(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return handle_redirect(req);
}

/*
 * The one-line routes below exist for one reason: get every OS's captive-
 * portal probe to land on "/" instead of quietly failing. Stage 2 (a plain
 * page on a real network) needs none of them.
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

esp_err_t ha_portal_run_wifi(void)
{
    esp_err_t err = bsp_wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "ha-tft-%02x%02x%02x", mac[3], mac[4], mac[5]);

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

    ha_setup_screen_show_wifi(ssid, s_redirect_url);

    ESP_LOGI(TAG, "Wi-Fi portal up: join \"%s\" (open), then visit %s", ssid, s_redirect_url);
    return ESP_OK;
}

/* ===========================================================================
 * Stage 2: Home Assistant config server, on the real station network
 * ========================================================================= */

static esp_err_t handle_config_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HA_PORTAL_CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
}

/*
 * What the stage-2 page prefills its form with, so a "reconfigure" doesn't
 * mean retyping everything. ws_token is deliberately reported as a boolean
 * ("already set?"), never its real value - unlike the rest of this form, a
 * secret already in NVS is not sent back to the browser to be displayed. See
 * copy_json_str_keep_if_blank()'s comment for how a blank resubmission is
 * then interpreted as "leave it as is".
 */
static esp_err_t handle_config_current(httpd_req_t *req)
{
    char ws_host[HA_WS_HOST_MAX]   = { 0 };
    char ws_token[HA_WS_TOKEN_MAX] = { 0 };

    ha_persist_get_str(HA_NVS_WS_HOST, ws_host, sizeof(ws_host));
    ha_persist_get_str(HA_NVS_WS_TOKEN, ws_token, sizeof(ws_token));
    const uint32_t ws_port = ha_persist_get_u32(HA_NVS_WS_PORT, HA_WS_PORT_DEFAULT);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ws_host", ws_host);
    cJSON_AddNumberToObject(root, "ws_port", ws_port);
    cJSON_AddBoolToObject(root, "ws_token_set", ws_token[0] != '\0');

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

    cJSON *root         = cJSON_Parse(body);
    free(body);
    cJSON *ws_host_item = root ? cJSON_GetObjectItemCaseSensitive(root, "ws_host") : NULL;

    /* The one field this firmware cannot do anything without: no host means
     * nothing to connect to. The token is allowed to be blank (some HA
     * setups front the WebSocket API with something that doesn't need one),
     * and the port falls back to the Kconfig default. */
    if (!cJSON_IsString(ws_host_item) || ws_host_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ws_host is required");
        return ESP_FAIL;
    }

    /*
     * Every destination buffer below is fixed-size and every copy goes
     * through strlcpy/parse_json_uint, so an over-long or malformed field is
     * truncated or clamped rather than overflowing anything.
     *
     * ws_token is seeded from whatever is already in NVS (empty on a
     * first-time save) before the JSON body is applied: the stage-2 page
     * never sends the real secret back to prefill the form (see
     * handle_config_current()), so an untouched, still-blank field in the
     * submission has to mean "keep the current value", not "clear it" - see
     * copy_json_str_keep_if_blank()'s own comment.
     */
    char ws_host[HA_WS_HOST_MAX]   = { 0 };
    char ws_token[HA_WS_TOKEN_MAX] = { 0 };

    ha_persist_get_str(HA_NVS_WS_TOKEN, ws_token, sizeof(ws_token));

    strlcpy(ws_host, ws_host_item->valuestring, sizeof(ws_host));
    copy_json_str_keep_if_blank(root, "ws_token", ws_token, sizeof(ws_token));

    const uint32_t ws_port = parse_json_uint(root, "ws_port",
                                             ha_persist_get_u32(HA_NVS_WS_PORT,
                                                                HA_WS_PORT_DEFAULT),
                                             1, 65535);
    cJSON_Delete(root);

    ha_persist_set_str(HA_NVS_WS_HOST, ws_host);
    ha_persist_set_u32(HA_NVS_WS_PORT, ws_port);
    ha_persist_set_str(HA_NVS_WS_TOKEN, ws_token);
    ha_persist_erase(HA_NVS_FORCE_CFG); /* settings are complete again */

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "ws_host", ws_host);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "configuration saved (Home Assistant at %s:%" PRIu32 "); rebooting",
             ws_host, ws_port);

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

esp_err_t ha_portal_run_config(void)
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

    ha_setup_screen_show_config(s_redirect_url);

    ESP_LOGI(TAG, "config server up at %s - waiting for Home Assistant setup", s_redirect_url);
    return ESP_OK;
}
