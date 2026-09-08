/*
 * Minimal HTTP(S) GET on top of esp_http_client, for fetching the dashboard
 * image from the user-configured URL.
 *
 * Bodies land in PSRAM: the image for a 1872x1404 panel can be several hundred
 * kilobytes, which internal RAM cannot hold alongside a TLS session.
 *
 * Unlike the trmnl-firmware example (which talks to one specific external
 * server and disables certificate verification for it by explicit, documented
 * decision), this module verifies the server certificate by default via the
 * mbedTLS certificate bundle (esp_crt_bundle_attach) whenever the URL is
 * https://. There is no insecure-by-default path here - see ha_http.c and the
 * README for how to opt out for a self-signed render-service host.
 */
#ifndef HA_HTTP_H
#define HA_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      status;            /**< HTTP status code, or -1 if none arrived   */
    char     content_type[96];  /**< empty when the server did not send one    */
    uint8_t *body;              /**< PSRAM, NUL-terminated, NULL when empty    */
    size_t   body_len;          /**< bytes, not counting the NUL terminator    */
    bool     truncated;         /**< true when the body hit the size limit     */
} ha_http_response_t;

/**
 * @brief  Perform an HTTP(S) GET and buffer the whole response body.
 *
 * Redirects are followed automatically.
 *
 * @param url          Absolute URL. http:// and https:// are both accepted.
 * @param bearer_token Sent as "Authorization: Bearer <token>" when non-empty;
 *                     pass NULL or "" to omit the header entirely (e.g. for a
 *                     render service that needs no auth).
 * @param max_body     Hard cap on the buffered body in bytes. Anything beyond
 *                     it is discarded and @p out->truncated is set.
 * @param timeout_ms   Per-transfer timeout.
 * @param out          Receives the result. Zeroed on entry.
 *
 * @return ESP_OK when a response was received, whatever its status code.
 *         Transport failures return the underlying esp_http_client error.
 */
esp_err_t ha_http_get(const char          *url,
                      const char          *bearer_token,
                      size_t               max_body,
                      int                  timeout_ms,
                      ha_http_response_t  *out);

/** @brief Free the body buffer and zero the struct. Safe on a zeroed struct. */
void ha_http_response_free(ha_http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* HA_HTTP_H */
