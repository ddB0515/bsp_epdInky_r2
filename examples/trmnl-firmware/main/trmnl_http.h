/*
 * Minimal HTTPS GET on top of esp_http_client.
 *
 * This replaces upstream's `lib/trmnl/include/http_client.h`, which is built on
 * Arduino HTTPClient + WiFiClientSecure. The shape is deliberately the same:
 * apply a header list, follow redirects, hand back a whole body, and report the
 * Content-Type so the caller can decide how to decode it.
 *
 * Bodies land in PSRAM. The image for a 1872x1404 panel can be several hundred
 * kilobytes, which internal RAM cannot hold alongside a TLS session.
 */

#ifndef TRMNL_HTTP_H
#define TRMNL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One request header. Both fields must outlive the trmnl_http_get() call. */
typedef struct {
    const char *name;
    const char *value;
} trmnl_http_header_t;

typedef struct {
    int      status;            /**< HTTP status code, or -1 if none arrived   */
    char     content_type[96];  /**< empty when the server did not send one    */
    uint8_t *body;              /**< PSRAM, NUL-terminated, NULL when empty    */
    size_t   body_len;          /**< bytes, not counting the NUL terminator    */
    bool     truncated;         /**< true when the body hit the size limit     */
} trmnl_http_response_t;

/**
 * @brief  Perform an HTTPS GET and buffer the whole response body.
 *
 * Redirects are followed automatically. Certificate verification is disabled,
 * matching upstream's setInsecure() - see the TLS section of
 * sdkconfig.defaults for how to tighten this.
 *
 * On success @p out owns a heap buffer; release it with
 * trmnl_http_response_free() whichever way the call went, since a partial body
 * is still returned when the transfer is truncated.
 *
 * @param url         Absolute URL. http:// and https:// are both accepted.
 * @param headers     Request headers, or NULL.
 * @param n_headers   Number of entries in @p headers.
 * @param max_body    Hard cap on the buffered body in bytes. Anything beyond it
 *                    is discarded and @p out->truncated is set.
 * @param timeout_ms  Per-transfer timeout.
 * @param out         Receives the result. Zeroed on entry.
 *
 * @return ESP_OK when a response was received, whatever its status code.
 *         Transport failures return the underlying esp_http_client error.
 */
esp_err_t trmnl_http_get(const char                *url,
                          const trmnl_http_header_t *headers,
                          size_t                     n_headers,
                          size_t                     max_body,
                          int                        timeout_ms,
                          trmnl_http_response_t     *out);

/**
 * @brief  Perform an HTTPS POST with a request body and buffer the response.
 *
 * Same shape and TLS policy as trmnl_http_get() - see its doc comment for
 * redirects, certificate handling and buffer ownership. @p body is read
 * synchronously during this call and does not need to outlive it.
 *
 * @param body      Request body, or NULL for none. Content-Length is set
 *                   from @p body_len automatically; Content-Type is not -
 *                   pass it in @p headers like any other.
 * @param body_len  Length of @p body in bytes.
 */
esp_err_t trmnl_http_post(const char                *url,
                           const trmnl_http_header_t *headers,
                           size_t                     n_headers,
                           const char                *body,
                           size_t                     body_len,
                           size_t                     max_body,
                           int                        timeout_ms,
                           trmnl_http_response_t     *out);

/** @brief Free the body buffer and zero the struct. Safe on a zeroed struct. */
void trmnl_http_response_free(trmnl_http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* TRMNL_HTTP_H */
