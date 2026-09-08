#include "ha_ws.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ha_config.h"

static const char *TAG = "ha_ws";

/* Ping/pong frames also arrive as WEBSOCKET_EVENT_DATA; everything else
 * (text and its continuations) is application payload. */
#define HA_WS_OPCODE_PING 0x09
#define HA_WS_OPCODE_PONG 0x0A

#define HA_WS_STAGE_TIMEOUT_MS 10000
#define HA_WS_PENDING_MAX      8

typedef enum {
    HA_WS_P_DISCONNECTED = 0,
    HA_WS_P_CONNECTING,
    HA_WS_P_WAIT_AUTH_REQUIRED,
    HA_WS_P_AUTH_SENT,
    HA_WS_P_SUBSCRIBING,
    HA_WS_P_FETCHING_STATES,
    HA_WS_P_READY,
} ha_ws_proto_state_t;

typedef enum {
    HA_WS_Q_CONNECTED,
    HA_WS_Q_DISCONNECTED,
    HA_WS_Q_MESSAGE,
} ha_ws_q_type_t;

typedef struct {
    ha_ws_q_type_t type;
    uint8_t       *payload; /* heap-owned (SPIRAM), only for HA_WS_Q_MESSAGE */
    size_t         len;
} ha_ws_q_item_t;

typedef struct {
    bool used;
    int  id;
    char domain[16];
    char service[24];
    char entity_id[HA_WS_ENTITY_ID_MAX];
} ha_ws_pending_t;

/* ── Config and tracked-entity list, set once by ha_ws_init() ─────────────── */

static char     s_host[HA_WS_HOST_MAX];
static uint16_t s_port;
static char     s_token[HA_WS_TOKEN_MAX];

static char   s_entity_ids[HA_WS_MAX_ENTITIES][HA_WS_ENTITY_ID_MAX];
static size_t s_entity_count;

/* ── Entity-state cache ────────────────────────────────────────────────────── */

static ha_ws_entity_state_t s_cache[HA_WS_MAX_ENTITIES];
static SemaphoreHandle_t    s_cache_lock;

/* ── call_service message-ID bookkeeping ──────────────────────────────────── */

static SemaphoreHandle_t s_id_lock;
static int               s_next_id = 1;

static SemaphoreHandle_t s_pending_lock;
static ha_ws_pending_t   s_pending[HA_WS_PENDING_MAX];

/* ── Callbacks ─────────────────────────────────────────────────────────────── */

static ha_ws_state_cb_t   s_state_cb;
static void               *s_state_cb_ctx;
static ha_ws_conn_cb_t    s_conn_cb;
static void               *s_conn_cb_ctx;
static ha_ws_conn_state_t s_conn_state = HA_WS_DISCONNECTED;

/* ── Protocol state, owned entirely by ha_ws_task ─────────────────────────── */

static esp_websocket_client_handle_t s_client;
static QueueHandle_t                 s_queue;
static volatile ha_ws_proto_state_t  s_proto = HA_WS_P_DISCONNECTED;
static volatile bool                 s_want_reconnect;
static int                           s_subscribe_id  = -1;
static int                           s_get_states_id = -1;
static uint32_t                      s_backoff_ms;

/* Single in-flight reassembly buffer - only touched from the WebSocket
 * client's own event-callback context, which esp_websocket_client serialises
 * onto one internal task, so this needs no lock of its own. */
static uint8_t *s_frag_buf;
static size_t   s_frag_len;
static size_t   s_frag_cap;

/* ===========================================================================
 * cJSON on PSRAM
 *
 * cJSON's allocator hooks are global to the whole link, not per-call.
 * get_states returns every entity in the Home Assistant install (no
 * server-side filter), which can be tens to hundreds of KB parsed in one
 * cJSON_Parse() call - routing that through PSRAM instead of internal RAM
 * avoids a transient internal-heap spike. This firmware has no other heavy
 * cJSON use, so the decision is made once, here.
 * ========================================================================= */

static void *cjson_spiram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void  cjson_spiram_free(void *p)      { heap_caps_free(p); }

/* ===========================================================================
 * Small helpers
 * ========================================================================= */

static int next_id(void)
{
    xSemaphoreTake(s_id_lock, portMAX_DELAY);
    int id = s_next_id++;
    xSemaphoreGive(s_id_lock);
    return id;
}

static int find_tracked_index(const char *entity_id)
{
    for (size_t i = 0; i < s_entity_count; i++) {
        if (strcmp(s_entity_ids[i], entity_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void set_conn_state(ha_ws_conn_state_t st)
{
    if (s_conn_state == st) {
        return;
    }
    s_conn_state = st;
    if (s_conn_cb) {
        s_conn_cb(st, s_conn_cb_ctx);
    }
}

static bool send_json(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return false;
    }
    int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json),
                                              pdMS_TO_TICKS(5000));
    cJSON_free(json);
    return sent >= 0;
}

static void record_pending(int id, const char *domain, const char *service, const char *entity_id)
{
    if (xSemaphoreTake(s_pending_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    int slot = -1;
    for (int i = 0; i < HA_WS_PENDING_MAX; i++) {
        if (!s_pending[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0; /* table full: evict the oldest tracked call rather than drop this one silently */
    }
    s_pending[slot].used = true;
    s_pending[slot].id   = id;
    strlcpy(s_pending[slot].domain, domain, sizeof(s_pending[slot].domain));
    strlcpy(s_pending[slot].service, service, sizeof(s_pending[slot].service));
    strlcpy(s_pending[slot].entity_id, entity_id, sizeof(s_pending[slot].entity_id));
    xSemaphoreGive(s_pending_lock);
}

static void log_pending_result(int id, bool ok)
{
    if (xSemaphoreTake(s_pending_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < HA_WS_PENDING_MAX; i++) {
        if (s_pending[i].used && s_pending[i].id == id) {
            if (!ok) {
                ESP_LOGW(TAG, "call_service %s.%s on %s failed",
                         s_pending[i].domain, s_pending[i].service, s_pending[i].entity_id);
            }
            s_pending[i].used = false;
            break;
        }
    }
    xSemaphoreGive(s_pending_lock);
}

/* ===========================================================================
 * Applying Home Assistant state objects to the cache
 * ========================================================================= */

static void apply_state_object(const cJSON *state_obj)
{
    /* A state_changed event's new_state is JSON null when the entity was
     * removed at runtime - nothing to apply, and definitely not something to
     * dereference as an object. */
    if (!cJSON_IsObject(state_obj)) {
        return;
    }

    const cJSON *entity_id = cJSON_GetObjectItemCaseSensitive(state_obj, "entity_id");
    const cJSON *state     = cJSON_GetObjectItemCaseSensitive(state_obj, "state");
    if (!cJSON_IsString(entity_id) || !cJSON_IsString(state)) {
        return;
    }

    int idx = find_tracked_index(entity_id->valuestring);
    if (idx < 0) {
        return; /* not one of this dashboard's tiles */
    }

    int brightness_pct = -1;
    const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(state_obj, "attributes");
    if (cJSON_IsObject(attrs)) {
        const cJSON *bri = cJSON_GetObjectItemCaseSensitive(attrs, "brightness");
        if (cJSON_IsNumber(bri)) {
            brightness_pct = (int)((bri->valuedouble * 100.0 / 255.0) + 0.5);
        }
    }

    if (xSemaphoreTake(s_cache_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strlcpy(s_cache[idx].state, state->valuestring, sizeof(s_cache[idx].state));
        s_cache[idx].brightness_pct = brightness_pct;
        s_cache[idx].have_state     = true;
        xSemaphoreGive(s_cache_lock);
    }

    if (s_state_cb) {
        ha_ws_entity_state_t out;
        if (ha_ws_get_state(s_entity_ids[idx], &out)) {
            s_state_cb(s_entity_ids[idx], &out, s_state_cb_ctx);
        }
    }
}

static void apply_get_states_result(const cJSON *result_array)
{
    if (!cJSON_IsArray(result_array)) {
        return;
    }
    /* get_states has no server-side filter - this walks every entity in the
     * whole Home Assistant install and cheaply discards the ones this
     * dashboard doesn't track. On an install with hundreds of entities that
     * is a real, if small, per-connect CPU cost - acceptable for a device
     * that reconnects occasionally, not something to optimise away in v1. */
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, result_array) {
        apply_state_object(item);
    }
}

/* ===========================================================================
 * Outgoing protocol messages
 * ========================================================================= */

static void send_auth(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "access_token", s_token);
    send_json(root);
    cJSON_Delete(root);
}

static void send_subscribe(int id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "subscribe_events");
    cJSON_AddStringToObject(root, "event_type", "state_changed");
    send_json(root);
    cJSON_Delete(root);
}

static void send_get_states(int id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "get_states");
    send_json(root);
    cJSON_Delete(root);
}

esp_err_t ha_ws_call_service(const char *domain, const char *service,
                             const char *entity_id, const ha_ws_service_data_t *data)
{
    if (!s_client || s_proto != HA_WS_P_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    int id = next_id();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", domain);
    cJSON_AddStringToObject(root, "service", service);
    cJSON *target = cJSON_AddObjectToObject(root, "target");
    cJSON_AddStringToObject(target, "entity_id", entity_id);
    if (data && data->brightness_pct >= 0) {
        cJSON *sd = cJSON_AddObjectToObject(root, "service_data");
        cJSON_AddNumberToObject(sd, "brightness_pct", data->brightness_pct);
    }

    record_pending(id, domain, service, entity_id);
    bool ok = send_json(root);
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ===========================================================================
 * Incoming protocol messages
 * ========================================================================= */

static void handle_message(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }

    if (strcmp(type->valuestring, "auth_required") == 0) {
        if (s_proto != HA_WS_P_WAIT_AUTH_REQUIRED) {
            return;
        }
        send_auth();
        s_proto = HA_WS_P_AUTH_SENT;
        return;
    }

    if (strcmp(type->valuestring, "auth_ok") == 0) {
        if (s_proto != HA_WS_P_AUTH_SENT) {
            return;
        }
        s_subscribe_id = next_id();
        send_subscribe(s_subscribe_id);
        s_proto = HA_WS_P_SUBSCRIBING;
        return;
    }

    if (strcmp(type->valuestring, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "Home Assistant rejected the access token - check the token saved "
                      "in Stage 2 setup");
        s_want_reconnect = true;
        return;
    }

    if (strcmp(type->valuestring, "result") == 0) {
        const cJSON *id_item  = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *success  = cJSON_GetObjectItemCaseSensitive(root, "success");
        int          id       = cJSON_IsNumber(id_item) ? id_item->valueint : -1;
        bool         ok       = cJSON_IsTrue(success);

        if (s_proto == HA_WS_P_SUBSCRIBING && id == s_subscribe_id) {
            if (!ok) {
                ESP_LOGE(TAG, "subscribe_events failed");
                s_want_reconnect = true;
                return;
            }
            s_get_states_id = next_id();
            send_get_states(s_get_states_id);
            s_proto = HA_WS_P_FETCHING_STATES;
            return;
        }

        if (s_proto == HA_WS_P_FETCHING_STATES && id == s_get_states_id) {
            if (!ok) {
                ESP_LOGE(TAG, "get_states failed");
                s_want_reconnect = true;
                return;
            }
            apply_get_states_result(cJSON_GetObjectItemCaseSensitive(root, "result"));
            s_proto      = HA_WS_P_READY;
            s_backoff_ms = 0; /* a full successful handshake resets the backoff ladder */
            set_conn_state(HA_WS_READY);
            ESP_LOGI(TAG, "Home Assistant WebSocket ready (%u tracked entities)",
                     (unsigned)s_entity_count);
            return;
        }

        /* Otherwise this is a call_service result - just log a failure with
         * context; v1 has no per-call UI feedback path (see ha_dashboard.c's
         * optimistic-tap-then-reconcile design). */
        log_pending_result(id, ok);
        return;
    }

    if (strcmp(type->valuestring, "event") == 0) {
        const cJSON *event      = cJSON_GetObjectItemCaseSensitive(root, "event");
        const cJSON *event_type = event ? cJSON_GetObjectItemCaseSensitive(event, "event_type") : NULL;
        if (!cJSON_IsString(event_type) || strcmp(event_type->valuestring, "state_changed") != 0) {
            return;
        }
        const cJSON *data      = cJSON_GetObjectItemCaseSensitive(event, "data");
        const cJSON *new_state = data ? cJSON_GetObjectItemCaseSensitive(data, "new_state") : NULL;
        apply_state_object(new_state);
        return;
    }
}

/* ===========================================================================
 * esp_websocket_client event callback
 *
 * Runs on the client's own internal task - never touches JSON, the cache, or
 * LVGL directly. It only reassembles fragmented payloads (get_states, and
 * occasionally a state_changed event with a large attributes dict, both
 * arrive split across multiple WEBSOCKET_EVENT_DATA calls) and hands
 * complete messages to ha_ws_task over a queue.
 * ========================================================================= */

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                             void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    ha_ws_q_item_t item = { 0 };

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        item.type = HA_WS_Q_CONNECTED;
        xQueueSend(s_queue, &item, 0);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        item.type = HA_WS_Q_DISCONNECTED;
        xQueueSend(s_queue, &item, 0);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == HA_WS_OPCODE_PING || data->op_code == HA_WS_OPCODE_PONG) {
            break;
        }
        if (data->payload_len <= 0 || data->data_len <= 0) {
            break;
        }

        if (data->payload_offset == 0) {
            /* Start of a new message - drop anything left over from a
             * previous, incompletely-reassembled one. */
            if (s_frag_buf) {
                heap_caps_free(s_frag_buf);
                s_frag_buf = NULL;
            }
            s_frag_cap = (size_t)data->payload_len;
            s_frag_buf = heap_caps_malloc(s_frag_cap + 1, MALLOC_CAP_SPIRAM);
            s_frag_len = 0;
            if (!s_frag_buf) {
                ESP_LOGE(TAG, "no memory for a %d-byte message", data->payload_len);
                break;
            }
        }
        if (!s_frag_buf) {
            break; /* the allocation above failed; drop the rest of this message */
        }
        if (s_frag_len + (size_t)data->data_len > s_frag_cap) {
            ESP_LOGE(TAG, "fragment overrun, dropping message");
            heap_caps_free(s_frag_buf);
            s_frag_buf = NULL;
            break;
        }

        memcpy(s_frag_buf + s_frag_len, data->data_ptr, (size_t)data->data_len);
        s_frag_len += (size_t)data->data_len;

        if (s_frag_len >= s_frag_cap) {
            s_frag_buf[s_frag_len] = '\0';
            item.type    = HA_WS_Q_MESSAGE;
            item.payload = s_frag_buf;
            item.len     = s_frag_len;
            s_frag_buf   = NULL; /* ownership transferred to the queue item */
            if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "queue full, dropping a message");
                heap_caps_free(item.payload);
            }
        }
        break;

    default:
        break;
    }
}

/* ===========================================================================
 * ha_ws_task: owns the protocol state machine and the reconnect policy
 * ========================================================================= */

static uint32_t backoff_next_ms(uint32_t cur)
{
    uint32_t next = cur ? cur * 2 : (uint32_t)CONFIG_HA_WS_RECONNECT_MIN_MS;
    if (next > (uint32_t)CONFIG_HA_WS_RECONNECT_MAX_MS) {
        next = (uint32_t)CONFIG_HA_WS_RECONNECT_MAX_MS;
    }
    /* +/- 20% jitter so many devices reconnecting to the same Home Assistant
     * instance after an outage don't all retry in lockstep. */
    uint32_t jitter = next / 5;
    if (jitter > 0) {
        next = next - jitter + (esp_random() % (2 * jitter + 1));
    }
    return next;
}

static void drain_queue(void)
{
    ha_ws_q_item_t item;
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        if (item.type == HA_WS_Q_MESSAGE && item.payload) {
            heap_caps_free(item.payload);
        }
    }
}

static void begin_connect_attempt(void)
{
    s_proto         = HA_WS_P_CONNECTING;
    s_subscribe_id  = -1;
    s_get_states_id = -1;
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_websocket_client_start: %s", esp_err_to_name(err));
    }
}

static void ha_ws_task(void *arg)
{
    (void)arg;

    set_conn_state(HA_WS_CONNECTING);
    begin_connect_attempt();

    while (true) {
        TickType_t timeout = (s_proto == HA_WS_P_READY) ? portMAX_DELAY
                                                        : pdMS_TO_TICKS(HA_WS_STAGE_TIMEOUT_MS);
        ha_ws_q_item_t item;
        BaseType_t got = xQueueReceive(s_queue, &item, timeout);
        bool reconnect = false;

        if (got != pdTRUE) {
            ESP_LOGW(TAG, "timed out waiting for the next handshake step (state %d)",
                     (int)s_proto);
            reconnect = true;
        } else if (item.type == HA_WS_Q_DISCONNECTED) {
            ESP_LOGW(TAG, "WebSocket disconnected");
            reconnect = true;
        } else if (item.type == HA_WS_Q_CONNECTED) {
            ESP_LOGI(TAG, "WebSocket connected, waiting for the Home Assistant auth challenge");
            s_proto = HA_WS_P_WAIT_AUTH_REQUIRED;
        } else { /* HA_WS_Q_MESSAGE */
            cJSON *root = cJSON_Parse((const char *)item.payload);
            heap_caps_free(item.payload);
            if (root) {
                s_want_reconnect = false;
                handle_message(root);
                cJSON_Delete(root);
                reconnect = s_want_reconnect;
            } else {
                ESP_LOGW(TAG, "could not parse a message as JSON");
            }
        }

        if (!reconnect) {
            continue;
        }

        set_conn_state(HA_WS_DISCONNECTED);
        s_proto = HA_WS_P_DISCONNECTED;
        esp_websocket_client_stop(s_client);
        drain_queue();

        s_backoff_ms = backoff_next_ms(s_backoff_ms);
        ESP_LOGI(TAG, "reconnecting in %" PRIu32 " ms", s_backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));

        set_conn_state(HA_WS_CONNECTING);
        begin_connect_attempt();
    }
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

esp_err_t ha_ws_init(const ha_ws_config_t *cfg, const char * const *entity_ids, size_t count)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->host && cfg->token, ESP_ERR_INVALID_ARG, TAG, "bad config");
    ESP_RETURN_ON_FALSE(count <= HA_WS_MAX_ENTITIES, ESP_ERR_INVALID_ARG, TAG,
                        "too many tracked entities (max %d)", HA_WS_MAX_ENTITIES);

    strlcpy(s_host, cfg->host, sizeof(s_host));
    s_port = cfg->port ? cfg->port : HA_WS_PORT_DEFAULT;
    strlcpy(s_token, cfg->token, sizeof(s_token));

    s_entity_count = count;
    for (size_t i = 0; i < count; i++) {
        strlcpy(s_entity_ids[i], entity_ids[i], sizeof(s_entity_ids[i]));
        memset(&s_cache[i], 0, sizeof(s_cache[i]));
        s_cache[i].brightness_pct = -1;
    }

    s_cache_lock   = xSemaphoreCreateMutex();
    s_pending_lock = xSemaphoreCreateMutex();
    s_id_lock      = xSemaphoreCreateMutex();
    s_queue        = xQueueCreate(8, sizeof(ha_ws_q_item_t));
    ESP_RETURN_ON_FALSE(s_cache_lock && s_pending_lock && s_id_lock && s_queue,
                        ESP_ERR_NO_MEM, TAG, "no memory for ha_ws state");

    static bool s_hooks_installed;
    if (!s_hooks_installed) {
        cJSON_Hooks hooks = {
            .malloc_fn = cjson_spiram_malloc,
            .free_fn   = cjson_spiram_free,
        };
        cJSON_InitHooks(&hooks);
        s_hooks_installed = true;
    }

    return ESP_OK;
}

esp_err_t ha_ws_start(void)
{
    static char s_uri[HA_WS_HOST_MAX + 32];
    snprintf(s_uri, sizeof(s_uri), "ws://%s:%u/api/websocket", s_host, (unsigned)s_port);

    esp_websocket_client_config_t ws_cfg = {
        .uri                    = s_uri,
        .disable_auto_reconnect = true, /* ha_ws_task drives reconnect itself */
        .buffer_size            = 4096,
    };
    s_client = esp_websocket_client_init(&ws_cfg);
    ESP_RETURN_ON_FALSE(s_client, ESP_FAIL, TAG, "could not create the WebSocket client");

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    BaseType_t ok = xTaskCreate(ha_ws_task, "ha_ws", 6144, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "could not start ha_ws_task");

    ESP_LOGI(TAG, "connecting to %s (%u tracked entities)", s_uri, (unsigned)s_entity_count);
    return ESP_OK;
}

void ha_ws_set_state_cb(ha_ws_state_cb_t cb, void *ctx)
{
    s_state_cb     = cb;
    s_state_cb_ctx = ctx;
}

void ha_ws_set_conn_cb(ha_ws_conn_cb_t cb, void *ctx)
{
    s_conn_cb     = cb;
    s_conn_cb_ctx = ctx;
}

bool ha_ws_get_state(const char *entity_id, ha_ws_entity_state_t *out)
{
    if (!out || !entity_id) {
        return false;
    }
    int idx = find_tracked_index(entity_id);
    if (idx < 0) {
        return false;
    }
    if (xSemaphoreTake(s_cache_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    *out = s_cache[idx];
    xSemaphoreGive(s_cache_lock);
    return true;
}
