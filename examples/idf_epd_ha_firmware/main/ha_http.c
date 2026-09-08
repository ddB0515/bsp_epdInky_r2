#include "ha_http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "ha_http";

/* Growth starts here and doubles. */
#define HA_HTTP_INITIAL_CAP 4096

typedef struct {
    ha_http_response_t *resp;
    size_t               cap;   /* bytes allocated, including the NUL slot */
    size_t               max;   /* caller's cap on body_len               */
} ha_http_ctx_t;

/*
 * Grow the body buffer so it can hold `need` bytes plus a NUL terminator.
 *
 * PSRAM first: a full-panel image is a few hundred kilobytes and would not fit
 * in internal RAM next to an open TLS session. heap_caps_realloc() moves the
 * block between heaps when it has to, so the internal fallback is safe even
 * after an earlier PSRAM allocation.
 */
static bool http_reserve(ha_http_ctx_t *ctx, size_t need)
{
    if (need + 1 <= ctx->cap) {
        return true;
    }

    size_t cap = ctx->cap ? ctx->cap : HA_HTTP_INITIAL_CAP;
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

static void http_reset_body(ha_http_ctx_t *ctx)
{
    ctx->resp->body_len        = 0;
    ctx->resp->truncated       = false;
    ctx->resp->content_type[0] = '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    ha_http_ctx_t *ctx = evt->user_data;

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
         * letting the body drain keeps the connection in a sane state and we
         * still learn the Content-Type and the magic bytes.
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
        /* A redirect's own body (and Content-Type, typically text/html) must
         * not be mistaken for the image it points at. */
        http_reset_body(ctx);
        break;

    default:
        break;
    }

    return ESP_OK;
}

esp_err_t ha_http_get(const char          *url,
                      const char          *bearer_token,
                      size_t               max_body,
                      int                  timeout_ms,
                      ha_http_response_t  *out)
{
    if (!url || !out || max_body == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status = -1;

    ha_http_ctx_t ctx = { .resp = out, .cap = 0, .max = max_body };

    const bool is_https = strncasecmp(url, "https://", 8) == 0;

    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = HTTP_METHOD_GET,
        .timeout_ms            = timeout_ms,
        .event_handler         = http_event_handler,
        .user_data             = &ctx,
        .buffer_size           = 2048,
        .buffer_size_tx        = 1024,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .keep_alive_enable     = false,
        /*
         * Verify the server certificate against the mbedTLS certificate
         * bundle whenever the URL is https:// - this is the opposite default
         * from the trmnl-firmware example, which turns verification off for
         * one specific, named external server by deliberate decision. Here
         * the URL is whatever the user typed into the captive portal, so
         * verifying it is the only sound default. If your render service or
         * broker has a self-signed certificate, see the README for how to
         * switch this to ESP_TLS_INSECURE for your own host rather than
         * disabling it globally.
         */
        .crt_bundle_attach     = is_https ? esp_crt_bundle_attach : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    if (bearer_token != NULL && bearer_token[0] != '\0') {
        /* "Bearer " is 7 bytes; the token itself is caller-owned and can be
         * long (a Home Assistant long-lived access token is a JWT), so build
         * the header value on the heap rather than guessing a stack size. */
        size_t needed = 7 + strlen(bearer_token) + 1;
        char *auth = malloc(needed);
        if (auth != NULL) {
            snprintf(auth, needed, "Bearer %s", bearer_token);
            esp_http_client_set_header(client, "Authorization", auth);
            free(auth);
        } else {
            ESP_LOGW(TAG, "no memory to build the Authorization header; sending the request without it");
        }
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGE(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }

    /* Always NUL-terminate: callers that sniff the body as text rely on it. */
    if (out->body && out->body_len < ctx.cap) {
        out->body[out->body_len] = '\0';
    }

    esp_http_client_cleanup(client);
    return err;
}

void ha_http_response_free(ha_http_response_t *resp)
{
    if (!resp) {
        return;
    }
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}
