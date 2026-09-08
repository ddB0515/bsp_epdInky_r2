#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "trmnl_http.h"

static const char *TAG = "trmnl_http";

/* Growth starts here and doubles; JSON replies never get past the first step. */
#define TRMNL_HTTP_INITIAL_CAP 4096

typedef struct {
    trmnl_http_response_t *resp;
    size_t                 cap;   /* bytes allocated, including the NUL slot */
    size_t                 max;   /* caller's cap on body_len               */
} trmnl_http_ctx_t;

/*
 * Grow the body buffer so it can hold `need` bytes plus a NUL terminator.
 *
 * PSRAM first: a full-panel image is a few hundred kilobytes and would not fit
 * in internal RAM next to an open TLS session. heap_caps_realloc() moves the
 * block between heaps when it has to, so the internal fallback is safe even
 * after an earlier PSRAM allocation.
 */
static bool http_reserve(trmnl_http_ctx_t *ctx, size_t need)
{
    if (need + 1 <= ctx->cap) {
        return true;
    }

    size_t cap = ctx->cap ? ctx->cap : TRMNL_HTTP_INITIAL_CAP;
    while (cap < need + 1) {
        cap *= 2;
    }
    if (cap > ctx->max + 1) {
        cap = ctx->max + 1;
    }

    uint8_t *buf = heap_caps_realloc(ctx->resp->body, cap,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_realloc(ctx->resp->body, cap, MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "out of memory growing response buffer to %u bytes",
                 (unsigned)cap);
        return false;
    }

    ctx->resp->body = buf;
    ctx->cap        = cap;
    return true;
}

static void http_reset_body(trmnl_http_ctx_t *ctx)
{
    ctx->resp->body_len          = 0;
    ctx->resp->truncated         = false;
    ctx->resp->content_type[0]   = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    trmnl_http_ctx_t *ctx = evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && strcasecmp(evt->header_key, "Content-Type") == 0 &&
            evt->header_value) {
            strlcpy(ctx->resp->content_type, evt->header_value,
                    sizeof(ctx->resp->content_type));
        }
        break;

    case HTTP_EVENT_ON_DATA: {
        if (evt->data_len <= 0) {
            break;
        }
        /*
         * Anything past the cap is dropped rather than aborting the transfer:
         * letting the body drain keeps the connection in a sane state and, for
         * the spike, we still learn the Content-Type and the magic bytes.
         */
        size_t room = ctx->max - ctx->resp->body_len;
        size_t take = (size_t)evt->data_len;
        if (take > room) {
            take = room;
            ctx->resp->truncated = true;
        }
        if (take == 0) {
            break;
        }
        if (!http_reserve(ctx, ctx->resp->body_len + take)) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(ctx->resp->body + ctx->resp->body_len, evt->data, take);
        ctx->resp->body_len += take;
        break;
    }

    case HTTP_EVENT_REDIRECT:
        /*
         * TRMNL hands back a presigned object-store URL for the image, so the
         * redirect body (and its Content-Type, typically text/html) must not be
         * mistaken for the image itself.
         */
        http_reset_body(ctx);
        break;

    default:
        break;
    }

    return ESP_OK;
}

/*
 * Shared by trmnl_http_get() and trmnl_http_post(): everything about a
 * request except the method and the (GET has none) body is identical -
 * same event handler, same buffer growth, same TLS policy.
 */
static esp_err_t trmnl_http_perform(esp_http_client_method_t   method,
                                    const char                *url,
                                    const trmnl_http_header_t *headers,
                                    size_t                     n_headers,
                                    const char                *body,
                                    size_t                     body_len,
                                    size_t                     max_body,
                                    int                        timeout_ms,
                                    trmnl_http_response_t     *out)
{
    if (!url || !out || max_body == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status = -1;

    trmnl_http_ctx_t ctx = { .resp = out, .cap = 0, .max = max_body };

    esp_http_client_config_t cfg = {
        .url                        = url,
        .method                     = method,
        .timeout_ms                 = timeout_ms,
        .event_handler              = http_event_handler,
        .user_data                  = &ctx,
        .buffer_size                = 2048,
        .buffer_size_tx             = 1024,
        .disable_auto_redirect      = false,
        .max_redirection_count      = 5,
        .keep_alive_enable          = false,
        /*
         * No CA and no bundle. Together with CONFIG_ESP_TLS_INSECURE and
         * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY this reproduces upstream's
         * WiFiClientSecure::setInsecure(). See sdkconfig.defaults.
         *
         * skip_cert_common_name_check must stay FALSE. It reads like it only
         * relaxes the CN check - which would be harmless here, since nothing is
         * verified anyway - but esp-tls implements it as
         * mbedtls_ssl_set_hostname(ssl, NULL), and that is also what installs
         * the SNI extension. Without SNI, trmnl.app's frontend cannot pick a
         * certificate and kills the handshake:
         *
         *   mbedtls_ssl_handshake returned -0x7780   (FATAL_ALERT_MESSAGE)
         *
         * Certificate verification is disabled elsewhere: esp-tls reaches
         * MBEDTLS_SSL_VERIFY_NONE because no CA, bundle or PSK is configured
         * above, on a code path independent of this flag.
         */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    for (size_t i = 0; i < n_headers; i++) {
        if (headers[i].name && headers[i].value) {
            esp_http_client_set_header(client, headers[i].name, headers[i].value);
        }
    }

    /* Sets Content-Length itself; Content-Type is the caller's job via headers,
     * same as it would be for any other header. */
    if (body != NULL && body_len > 0) {
        esp_http_client_set_post_field(client, body, (int)body_len);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGE(TAG, "%s %s failed: %s",
                 method == HTTP_METHOD_POST ? "POST" : "GET", url, esp_err_to_name(err));
    }

    /* Always NUL-terminate: the JSON parsers treat the body as a C string. */
    if (out->body && out->body_len < ctx.cap) {
        out->body[out->body_len] = '\0';
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t trmnl_http_get(const char                *url,
                          const trmnl_http_header_t *headers,
                          size_t                     n_headers,
                          size_t                     max_body,
                          int                        timeout_ms,
                          trmnl_http_response_t     *out)
{
    return trmnl_http_perform(HTTP_METHOD_GET, url, headers, n_headers,
                              NULL, 0, max_body, timeout_ms, out);
}

esp_err_t trmnl_http_post(const char                *url,
                           const trmnl_http_header_t *headers,
                           size_t                     n_headers,
                           const char                *body,
                           size_t                     body_len,
                           size_t                     max_body,
                           int                        timeout_ms,
                           trmnl_http_response_t     *out)
{
    return trmnl_http_perform(HTTP_METHOD_POST, url, headers, n_headers,
                              body, body_len, max_body, timeout_ms, out);
}

void trmnl_http_response_free(trmnl_http_response_t *resp)
{
    if (!resp) {
        return;
    }
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}
