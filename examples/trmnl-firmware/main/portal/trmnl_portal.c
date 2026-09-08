#include "trmnl_portal.h"

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

#include "trmnl_config.h"
#include "trmnl_display.h"
#include "trmnl_dns.h"
#include "trmnl_persist.h"
#include "trmnl_portal_page.h"

static const char *TAG = "trmnl_portal";

/* Matches upstream's WIFI_CHANNEL - both are an arbitrary-but-common 2.4 GHz
 * mid-band channel, no reason to pick a different one. */
#define TRMNL_PORTAL_WIFI_CHANNEL 6

/* "Prevent dead batteries" - upstream's own reasoning for not holding a
 * SoftAP up indefinitely. Reused for the retry sleep too: there's no signal
 * calling for a different number, and one constant is one thing to explain. */
#define TRMNL_PORTAL_TIMEOUT_MS   (15 * 60 * 1000)
#define TRMNL_PORTAL_RETRY_SLEEP_S (15 * 60)

#define TRMNL_PORTAL_BODY_MAX 512
#define TRMNL_PORTAL_SCAN_MAX 16

static char s_ap_ip[16];
static char s_redirect_url[32];

/* ===========================================================================
 * Route handlers
 * ========================================================================= */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TRMNL_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    bsp_wifi_ap_t aps[TRMNL_PORTAL_SCAN_MAX];
    size_t        found = 0;

    /* Errors here just mean an empty list - typing the SSID in by hand still
     * works, so there is nothing worth failing the request over. */
    bsp_wifi_scan(aps, TRMNL_PORTAL_SCAN_MAX, &found);

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

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_connect(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= TRMNL_PORTAL_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    char body[TRMNL_PORTAL_BODY_MAX];
    int  received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    cJSON *root      = cJSON_Parse(body);
    cJSON *ssid_item = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;
    cJSON *pswd_item = root ? cJSON_GetObjectItemCaseSensitive(root, "pswd") : NULL;

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    char ssid[TRMNL_WIFI_SSID_MAX];
    char pswd[TRMNL_WIFI_PASS_MAX];
    strlcpy(ssid, ssid_item->valuestring, sizeof(ssid));
    strlcpy(pswd, (cJSON_IsString(pswd_item) && pswd_item->valuestring) ? pswd_item->valuestring : "",
            sizeof(pswd));
    cJSON_Delete(root);

    trmnl_persist_set_str(TRMNL_NVS_WIFI_SSID, ssid);
    trmnl_persist_set_str(TRMNL_NVS_WIFI_PASS, pswd);

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "ssid", ssid);
    cJSON_AddStringToObject(resp, "mac", mac_str);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    cJSON_Delete(resp);

    ESP_LOGI(TAG, "credentials saved for \"%s\"; rebooting", ssid);

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
 * portal's own address - see WebServer.cpp's comment on the same route. */
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
 * portal probe to land on "/" instead of quietly failing. Kept close to
 * WebServer.cpp's own set and behaviour rather than trimmed to a guess at
 * what's "really" needed - these are hard-won per-OS quirks (why
 * /wpad.dat is a 404 and not a redirect, why /connecttest.txt goes
 * elsewhere entirely), not padding.
 */
static const httpd_uri_t s_get_routes[] = {
    { .uri = "/",                    .method = HTTP_GET, .handler = handle_root },
    { .uri = "/scan",                .method = HTTP_GET, .handler = handle_scan },
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

static const httpd_uri_t s_post_routes[] = {
    { .uri = "/connect", .method = HTTP_POST, .handler = handle_connect },
};

static void register_routes(httpd_handle_t server)
{
    for (size_t i = 0; i < sizeof(s_get_routes) / sizeof(s_get_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_get_routes[i]);
    }
    for (size_t i = 0; i < sizeof(s_post_routes) / sizeof(s_post_routes[0]); i++) {
        httpd_register_uri_handler(server, &s_post_routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_404_err);
}

/* ===========================================================================
 * Orchestration
 * ========================================================================= */

esp_err_t trmnl_portal_run(const bsp_epdinky_handles_t *board)
{
    esp_err_t err = bsp_wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ssid[24];
    snprintf(ssid, sizeof(ssid), "TRMNL-%02X%02X%02X", mac[3], mac[4], mac[5]);

    err = bsp_wifi_ap_start(ssid, NULL, TRMNL_PORTAL_WIFI_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        return err;
    }

    bsp_wifi_ap_get_ip_str(s_ap_ip, sizeof(s_ap_ip));
    snprintf(s_redirect_url, sizeof(s_redirect_url), "http://%s/", s_ap_ip);

    trmnl_dns_start((uint32_t)inet_addr(s_ap_ip));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size       = 6144; /* scan results + cJSON on top of the default */
    config.lru_purge_enable = true;

    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        trmnl_dns_stop();
        return err;
    }
    register_routes(server);

    trmnl_display_show_wifi_setup(ssid, TRMNL_FW_VERSION_STRING);

    ESP_LOGI(TAG, "portal up: join \"%s\" (open), then visit %s", ssid, s_redirect_url);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TRMNL_PORTAL_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "portal timed out after %d minutes with nobody provisioning; sleeping",
             TRMNL_PORTAL_TIMEOUT_MS / 60000);

    httpd_stop(server);
    trmnl_dns_stop();
    bsp_wifi_deinit();
    if (board && board->tps65185) {
        tps65185_sleep(board->tps65185);
    }

    esp_sleep_enable_timer_wakeup((uint64_t)TRMNL_PORTAL_RETRY_SLEEP_S * 1000000ULL);
    esp_deep_sleep_start();
    return ESP_OK; /* not reached */
}
