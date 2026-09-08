/*
 * Home Assistant WebSocket client.
 *
 * Talks to Home Assistant's native ws://<host>:<port>/api/websocket API
 * directly - auth with a long-lived access token, subscribe_events for
 * state_changed, an initial get_states fetch, call_service for actions -
 * rather than through MQTT, so arbitrary entities work without any extra
 * Home-Assistant-side configuration.
 *
 * Owns its own task and reconnects indefinitely (HA restarts routinely, e.g.
 * after an update) with capped exponential backoff, re-subscribing and
 * re-fetching state after every reconnect. Never touches LVGL - ha_dashboard.c
 * is the only caller that knows LVGL exists.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw HA state string, e.g. "on"/"off"/"unavailable"/"23.4". */
#define HA_WS_STATE_STR_MAX 16
#define HA_WS_ENTITY_ID_MAX 48

/* Bounded by the compiled-in tile list (ha_dashboard_config.h) - this is not
 * meant to track every entity in the Home Assistant install, just the ones
 * shown on this dashboard. */
#define HA_WS_MAX_ENTITIES 32

typedef struct {
    const char *host;   /* hostname or IP, no scheme, e.g. "homeassistant.local" */
    uint16_t    port;   /* typically 8123 */
    const char *token;  /* long-lived access token */
} ha_ws_config_t;

typedef enum {
    HA_WS_DISCONNECTED = 0,
    HA_WS_CONNECTING,
    HA_WS_READY,
} ha_ws_conn_state_t;

typedef struct {
    char state[HA_WS_STATE_STR_MAX];
    int  brightness_pct;  /* 0-100, -1 = not reported/not applicable */
    bool have_state;       /* false until a real update has been seen */
} ha_ws_entity_state_t;

typedef struct {
    int brightness_pct; /* -1 = omit from the service call */
} ha_ws_service_data_t;

typedef void (*ha_ws_state_cb_t)(const char *entity_id, const ha_ws_entity_state_t *st, void *ctx);
typedef void (*ha_ws_conn_cb_t)(ha_ws_conn_state_t state, void *ctx);

/**
 * @brief  Configure the client and the set of entities to track.
 *
 * @param  entity_ids  array of @p count entity_id strings; copied internally,
 *                      the caller's storage need not outlive this call.
 */
esp_err_t ha_ws_init(const ha_ws_config_t *cfg, const char * const *entity_ids, size_t count);

/** @brief Start the client task and the first connection attempt. Non-blocking. */
esp_err_t ha_ws_start(void);

/** @brief Register the callback fired (from ha_ws's own task) on every tracked
 *  entity's state_changed event, and once per entity right after get_states. */
void ha_ws_set_state_cb(ha_ws_state_cb_t cb, void *ctx);

/** @brief Register the callback fired (from ha_ws's own task) on every
 *  connect/disconnect transition. */
void ha_ws_set_conn_cb(ha_ws_conn_cb_t cb, void *ctx);

/** @brief Copy out the last-known state for @p entity_id. Returns false if
 *  the entity isn't tracked at all (not in the list passed to ha_ws_init()). */
bool ha_ws_get_state(const char *entity_id, ha_ws_entity_state_t *out);

/**
 * @brief  Call a Home Assistant service. Safe to call from any task,
 *         including directly from an LVGL tap-event handler.
 *
 * @return ESP_ERR_INVALID_STATE if not currently connected (nothing sent) -
 *         the caller should treat this the same as a call that was sent but
 *         failed, since no state_changed confirmation will follow either way.
 */
esp_err_t ha_ws_call_service(const char *domain, const char *service,
                             const char *entity_id, const ha_ws_service_data_t *data);

#ifdef __cplusplus
}
#endif
