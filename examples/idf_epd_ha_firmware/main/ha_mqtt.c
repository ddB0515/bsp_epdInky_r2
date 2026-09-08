#include "ha_mqtt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "ha_config.h"

static const char *TAG = "ha_mqtt";

#define HA_MQTT_CONNECT_TIMEOUT_MS 15000
#define HA_MQTT_TOPIC_MAX          160

/* -------------------------------------------------------------------------- */
/* Connection state, for the duration of one ha_mqtt_run_cycle() call         */
/* -------------------------------------------------------------------------- */

typedef struct {
    volatile bool connected;
    volatile bool gave_up;             /* disconnected/errored before ever connecting */
    volatile bool refresh_requested;
    char          command_topic[HA_MQTT_TOPIC_MAX];
} ha_mqtt_runtime_t;

static ha_mqtt_runtime_t s_rt;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_rt.connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        if (!s_rt.connected) {
            /* Never got in - most likely the broker is unreachable, refused
             * the credentials, or the host/port in NVS is wrong. */
            s_rt.gave_up = true;
        }
        break;

    case MQTT_EVENT_ERROR:
        if (!s_rt.connected) {
            s_rt.gave_up = true;
        }
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && s_rt.command_topic[0] != '\0' &&
            (size_t)event->topic_len == strlen(s_rt.command_topic) &&
            strncmp(event->topic, s_rt.command_topic, (size_t)event->topic_len) == 0) {
            ESP_LOGI(TAG, "refresh command received");
            s_rt.refresh_requested = true;
        }
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Identity                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t ha_mqtt_device_id(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(out, len, "ha_epdinky_%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Topics                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * State/command topics live under "ha_epdinky/<device_id>/...", separate
 * from the "homeassistant/" discovery prefix that only ever carries the
 * retained config payloads. Keeping the two apart means a user who changes
 * Home Assistant's discovery prefix (Settings -> Devices & Services -> MQTT
 * -> configure) only affects where the config topics are published, not
 * where this device's own data lives.
 */
static void topic_avail(char *out, size_t len, const char *device_id)
{
    snprintf(out, len, "ha_epdinky/%s/availability", device_id);
}

static void topic_state(char *out, size_t len, const char *device_id, const char *object_id)
{
    snprintf(out, len, "ha_epdinky/%s/%s/state", device_id, object_id);
}

static void topic_command(char *out, size_t len, const char *device_id, const char *object_id)
{
    snprintf(out, len, "ha_epdinky/%s/%s/set", device_id, object_id);
}

static void topic_discovery(char *out, size_t len, const char *component,
                            const char *device_id, const char *object_id)
{
    snprintf(out, len, "homeassistant/%s/%s/%s/config", component, device_id, object_id);
}

/* -------------------------------------------------------------------------- */
/* Discovery payloads                                                        */
/* -------------------------------------------------------------------------- */

static void add_device_block(cJSON *root, const char *device_id)
{
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON *ids    = cJSON_AddArrayToObject(device, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(device_id));
    cJSON_AddStringToObject(device, "name", "epdInky Dashboard");
    cJSON_AddStringToObject(device, "model", "epdInky ESP32-P4/C6");
    cJSON_AddStringToObject(device, "sw_version", HA_FW_VERSION_STRING);
}

/** One sensor/binary_sensor discovery config. @p unit, @p device_class and
 *  @p state_class may each be NULL to omit that field. @p expire_after_s, if
 *  nonzero, becomes the entity's `expire_after` - see
 *  ha_mqtt_config_t::state_max_age_s for what that's for. */
static esp_err_t publish_sensor_discovery(esp_mqtt_client_handle_t client,
                                          const char *component,
                                          const char *device_id,
                                          const char *object_id,
                                          const char *name,
                                          const char *unit,
                                          const char *device_class,
                                          const char *state_class,
                                          const char *availability_topic,
                                          uint32_t    expire_after_s)
{
    char disc_topic[HA_MQTT_TOPIC_MAX];
    char state_topic[HA_MQTT_TOPIC_MAX];
    char unique_id[HA_MQTT_TOPIC_MAX];

    topic_discovery(disc_topic, sizeof(disc_topic), component, device_id, object_id);
    topic_state(state_topic, sizeof(state_topic), device_id, object_id);
    snprintf(unique_id, sizeof(unique_id), "%s_%s", device_id, object_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    if (unit != NULL) {
        cJSON_AddStringToObject(root, "unit_of_measurement", unit);
    }
    if (device_class != NULL) {
        cJSON_AddStringToObject(root, "device_class", device_class);
    }
    if (state_class != NULL) {
        cJSON_AddStringToObject(root, "state_class", state_class);
    }
    if (expire_after_s != 0) {
        cJSON_AddNumberToObject(root, "expire_after", expire_after_s);
    }
    add_device_block(root, device_id);

    char *json = cJSON_PrintUnformatted(root);
    int msg_id = -1;
    if (json != NULL) {
        msg_id = esp_mqtt_client_publish(client, disc_topic, json, 0, 1, 1 /* retain */);
        cJSON_free(json);
    }
    cJSON_Delete(root);

    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t publish_button_discovery(esp_mqtt_client_handle_t client,
                                          const char *device_id,
                                          const char *availability_topic)
{
    char disc_topic[HA_MQTT_TOPIC_MAX];
    char cmd_topic[HA_MQTT_TOPIC_MAX];
    char unique_id[HA_MQTT_TOPIC_MAX];

    topic_discovery(disc_topic, sizeof(disc_topic), "button", device_id, "refresh");
    topic_command(cmd_topic, sizeof(cmd_topic), device_id, "refresh");
    snprintf(unique_id, sizeof(unique_id), "%s_refresh", device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Refresh Now");
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "command_topic", cmd_topic);
    cJSON_AddStringToObject(root, "payload_press", "REFRESH");
    cJSON_AddStringToObject(root, "availability_topic", availability_topic);
    add_device_block(root, device_id);

    char *json = cJSON_PrintUnformatted(root);
    int msg_id = -1;
    if (json != NULL) {
        msg_id = esp_mqtt_client_publish(client, disc_topic, json, 0, 1, 1 /* retain */);
        cJSON_free(json);
    }
    cJSON_Delete(root);

    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

static void publish_state_str(esp_mqtt_client_handle_t client, const char *device_id,
                              const char *object_id, const char *value)
{
    char topic[HA_MQTT_TOPIC_MAX];
    topic_state(topic, sizeof(topic), device_id, object_id);
    /* Retained: this device is reachable for only a few seconds per cycle, so
     * Home Assistant should keep showing the last known reading rather than
     * "unknown" for the rest of the sleep interval. */
    esp_mqtt_client_publish(client, topic, value, 0, 1, 1);
}

static void publish_state_f(esp_mqtt_client_handle_t client, const char *device_id,
                            const char *object_id, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", (double)value);
    publish_state_str(client, device_id, object_id, buf);
}

/* -------------------------------------------------------------------------- */
/* Orchestration                                                              */
/* -------------------------------------------------------------------------- */

esp_err_t ha_mqtt_run_cycle(const ha_mqtt_config_t *cfg,
                            const ha_mqtt_state_t  *state,
                            uint32_t                command_window_s,
                            bool                   *out_refresh_requested)
{
    if (out_refresh_requested != NULL) {
        *out_refresh_requested = false;
    }
    if (cfg == NULL || state == NULL || cfg->host == NULL || cfg->host[0] == '\0' ||
        cfg->device_id == NULL || cfg->device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_rt, 0, sizeof(s_rt));

    char avail_topic[HA_MQTT_TOPIC_MAX];
    topic_avail(avail_topic, sizeof(avail_topic), cfg->device_id);
    topic_command(s_rt.command_topic, sizeof(s_rt.command_topic), cfg->device_id, "refresh");

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = cfg->host,
        .broker.address.port     = cfg->port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id   = cfg->device_id,
        .credentials.username    = (cfg->username != NULL && cfg->username[0] != '\0')
                                    ? cfg->username : NULL,
        .credentials.authentication.password = (cfg->password != NULL && cfg->password[0] != '\0')
                                    ? cfg->password : NULL,
        .session.last_will = {
            .topic   = avail_topic,
            .msg     = "offline",
            .msg_len = 0, /* NUL-terminated */
            .qos     = 1,
            .retain  = 1,
        },
        .network.disable_auto_reconnect = true,
        .network.timeout_ms             = 10000,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        return err;
    }

    /* Block until connected, refused, or the timeout - esp-mqtt runs its own
     * task and delivers everything through mqtt_event_handler() above. */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HA_MQTT_CONNECT_TIMEOUT_MS);
    while (!s_rt.connected && !s_rt.gave_up && xTaskGetTickCount() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!s_rt.connected) {
        ESP_LOGW(TAG, "could not connect to %s:%" PRIu16, cfg->host, cfg->port);
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        return ESP_ERR_TIMEOUT;
    }

    /* ---- Announce ------------------------------------------------------- */

    esp_mqtt_client_publish(client, avail_topic, "online", 0, 1, 1);

    /* ---- Discovery -------------------------------------------------------
     *
     * Republished every cycle rather than once ever: these are retained, so
     * the broker keeps only the latest copy, and re-announcing costs one
     * small publish per entity against the alternative of tracking "have we
     * announced before" somewhere durable for no real benefit.
     */
    const uint32_t expire_after_s = cfg->state_max_age_s;

    publish_sensor_discovery(client, "sensor", cfg->device_id, "battery_voltage",
                             "Battery Voltage", "V", "voltage", "measurement", avail_topic,
                             expire_after_s);
    if (state->battery_present) {
        /* soc_permille from the STC3115 gauge - a real gas-gauge estimate,
         * not a value this firmware derives from a voltage curve. Only
         * announced when a battery is actually present. */
        publish_sensor_discovery(client, "sensor", cfg->device_id, "battery_percent",
                                 "Battery", "%", "battery", "measurement", avail_topic,
                                 expire_after_s);
    }
    publish_sensor_discovery(client, "binary_sensor", cfg->device_id, "battery_charging",
                             "Battery Charging", NULL, "battery_charging", NULL, avail_topic,
                             expire_after_s);
    publish_sensor_discovery(client, "sensor", cfg->device_id, "wifi_rssi",
                             "Wi-Fi Signal", "dBm", "signal_strength", "measurement", avail_topic,
                             expire_after_s);
    publish_sensor_discovery(client, "sensor", cfg->device_id, "last_refresh",
                             "Last Refresh", NULL, "timestamp", NULL, avail_topic,
                             expire_after_s);
    publish_button_discovery(client, cfg->device_id, avail_topic);

    /* ---- State ------------------------------------------------------------ */

    publish_state_f(client, cfg->device_id, "battery_voltage", state->battery_voltage_v);
    if (state->battery_present) {
        publish_state_f(client, cfg->device_id, "battery_percent", state->battery_percent);
    }
    publish_state_str(client, cfg->device_id, "battery_charging",
                      state->battery_charging ? "ON" : "OFF");
    publish_state_f(client, cfg->device_id, "wifi_rssi", (float)state->wifi_rssi_dbm);
    if (state->last_refresh_iso8601 != NULL && state->last_refresh_iso8601[0] != '\0') {
        publish_state_str(client, cfg->device_id, "last_refresh", state->last_refresh_iso8601);
    }

    /* ---- Listen briefly for a refresh command ----------------------------- */

    esp_mqtt_client_subscribe(client, s_rt.command_topic, 1);

    const TickType_t listen_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(command_window_s * 1000u);
    while (!s_rt.refresh_requested && xTaskGetTickCount() < listen_deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (out_refresh_requested != NULL) {
        *out_refresh_requested = s_rt.refresh_requested;
    }

    /* ---- Disconnect --------------------------------------------------------
     *
     * Deliberately *not* publishing "offline" here (see the header comment):
     * the availability topic stays latched "online" - retained - through this
     * ordinary planned disconnect, and each entity's own `expire_after`
     * (state_max_age_s) is what marks it stale if the device actually misses
     * check-ins, rather than every single sleep looking identical to a fault.
     * The LWT is still armed for a genuine crash or dropped connection.
     */
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Idle-wait listener                                                        */
/* -------------------------------------------------------------------------- */

static esp_mqtt_client_handle_t s_listen_client;
static volatile bool           *s_listen_flag;
static char                     s_listen_command_topic[HA_MQTT_TOPIC_MAX];

static void listen_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        /* Subscribing here rather than blocking the caller for it - this
         * listener runs alongside the idle wait, not before it. */
        esp_mqtt_client_subscribe(s_listen_client, s_listen_command_topic, 1);
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && s_listen_command_topic[0] != '\0' &&
            (size_t)event->topic_len == strlen(s_listen_command_topic) &&
            strncmp(event->topic, s_listen_command_topic, (size_t)event->topic_len) == 0) {
            ESP_LOGI(TAG, "refresh command received during idle wait");
            if (s_listen_flag != NULL) {
                *s_listen_flag = true;
            }
        }
        break;

    default:
        break;
    }
}

esp_err_t ha_mqtt_listen_start(const ha_mqtt_config_t *cfg, volatile bool *out_refresh_requested)
{
    if (cfg == NULL || cfg->host == NULL || cfg->host[0] == '\0' ||
        cfg->device_id == NULL || cfg->device_id[0] == '\0' || out_refresh_requested == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_listen_flag = out_refresh_requested;
    topic_command(s_listen_command_topic, sizeof(s_listen_command_topic), cfg->device_id, "refresh");

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = cfg->host,
        .broker.address.port     = cfg->port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id   = cfg->device_id,
        .credentials.username    = (cfg->username != NULL && cfg->username[0] != '\0')
                                    ? cfg->username : NULL,
        .credentials.authentication.password = (cfg->password != NULL && cfg->password[0] != '\0')
                                    ? cfg->password : NULL,
        .network.timeout_ms      = 10000,
        /* Auto-reconnect stays on (unlike ha_mqtt_run_cycle()'s client): this
         * listener can be up for the whole refresh interval, and a broker
         * blip partway through shouldn't permanently end its only chance to
         * catch a press. */
    };

    s_listen_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_listen_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (listener)");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_listen_client, ESP_EVENT_ANY_ID, listen_event_handler, NULL);

    esp_err_t err = esp_mqtt_client_start(s_listen_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed (listener): %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_listen_client);
        s_listen_client = NULL;
        return err;
    }

    return ESP_OK;
}

void ha_mqtt_listen_stop(void)
{
    if (s_listen_client != NULL) {
        esp_mqtt_client_stop(s_listen_client);
        esp_mqtt_client_destroy(s_listen_client);
        s_listen_client = NULL;
    }
    s_listen_flag = NULL;
}
