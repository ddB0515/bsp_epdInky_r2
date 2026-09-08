#include "trmnl_battery.h"

#include "esp_log.h"

static const char *TAG = "trmnl_battery";

/*
 * Below any real Li-ion cell (nominal range is roughly 3.0-4.2 V) - a second
 * angle on the same "no battery" condition BATFAIL already covers, not a
 * second unrelated threshold. Catches a disconnected or floating VBAT that
 * somehow didn't trip BATFAIL (UVLO < 2.6 V) on a given read.
 */
#define TRMNL_BATTERY_MIN_PLAUSIBLE_MV 2000

static stc3115_handle_t s_handle;

esp_err_t trmnl_battery_init(stc3115_handle_t handle)
{
    s_handle = handle;
    return handle ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t trmnl_battery_read(trmnl_battery_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->voltage_v = 0.0f;
    out->present   = false;
    out->charging  = false;

    if (s_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    stc3115_data_t data;
    esp_err_t err = stc3115_read_data(s_handle, &data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        return err;
    }

    out->present = !data.battery_fail && data.voltage_mv >= TRMNL_BATTERY_MIN_PLAUSIBLE_MV;
    out->voltage_v = data.voltage_mv / 1000.0f;
    /* FULLY_CHARGED still counts: on this board it only reads that way while
     * charge power is holding the battery there, so it is still "plugged
     * in" in the sense the server's Battery-Charging header cares about. */
    out->charging = (data.charge_status == STC3115_CHARGING ||
                     data.charge_status == STC3115_FULLY_CHARGED);

    if (!out->present) {
        ESP_LOGW(TAG, "no battery detected (battery_fail=%d, voltage=%u mV)",
                 (int)data.battery_fail, (unsigned)data.voltage_mv);
    }

    return ESP_OK;
}
