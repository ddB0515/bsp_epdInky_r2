#include "ha_persist.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "ha_config.h"

static const char *TAG = "ha_persist";

static nvs_handle_t s_nvs;
static bool         s_open;

esp_err_t ha_persist_init(void)
{
    if (s_open) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as it stands: either full of dead entries
         * or written by a newer NVS. Both are recoverable only by erasing,
         * which just means re-provisioning through the captive portal. */
        ESP_LOGW(TAG, "NVS needs erasing (%s); re-initialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(HA_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s): %s", HA_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    s_open = true;
    return ESP_OK;
}

bool ha_persist_exists(const char *key)
{
    if (!s_open || key == NULL) {
        return false;
    }

    /* nvs_find_key() reports the type without reading the value, which is
     * the only way to tell "stored as 0" from "never stored" for numeric
     * keys such as HA_NVS_MQTT_PORT. */
    nvs_type_t type;
    return nvs_find_key(s_nvs, key, &type) == ESP_OK;
}

esp_err_t ha_persist_get_str(const char *key, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!s_open || key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t    have = len;
    esp_err_t err  = nvs_get_str(s_nvs, key, out, &have);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;      /* absent is not a failure; out is already empty */
    }
    if (err != ESP_OK) {
        out[0] = '\0';
        ESP_LOGW(TAG, "read \"%s\": %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t ha_persist_set_str(const char *key, const char *value)
{
    if (!s_open || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_str(s_nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write \"%s\": %s", key, esp_err_to_name(err));
    }
    return err;
}

uint32_t ha_persist_get_u32(const char *key, uint32_t fallback)
{
    if (!s_open || key == NULL) {
        return fallback;
    }

    uint32_t value = 0;
    if (nvs_get_u32(s_nvs, key, &value) != ESP_OK) {
        return fallback;
    }
    return value;
}

esp_err_t ha_persist_set_u32(const char *key, uint32_t value)
{
    if (!s_open || key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u32(s_nvs, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write \"%s\": %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t ha_persist_erase(const char *key)
{
    if (!s_open || key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}
