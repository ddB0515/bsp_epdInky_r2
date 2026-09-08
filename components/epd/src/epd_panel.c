#include "epd_panel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include <stdlib.h>

static const char *TAG = "epd_panel";

struct epd_panel_dev {
    epd_panel_config_t   cfg;
    epd_panel_ops_t      ops;
    epd_i80_bus_handle_t bus;
    void                *priv;  /**< panel-driver private data, set via ops->init */
};

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

esp_err_t epd_panel_create(const epd_panel_config_t *config,
                            const epd_panel_ops_t    *ops,
                            epd_panel_handle_t       *handle)
{
    ESP_RETURN_ON_FALSE(config && ops && handle,
                        ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(config->pmic,
                        ESP_ERR_INVALID_ARG, TAG, "PMIC handle required");
    ESP_RETURN_ON_FALSE(ops->power_on && ops->power_off && ops->refresh,
                        ESP_ERR_INVALID_ARG, TAG, "ops table incomplete");

    struct epd_panel_dev *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "alloc failed");

    dev->cfg = *config;
    dev->ops = *ops;

    esp_err_t ret = epd_i80_bus_init(&config->bus_cfg, config->bus_width, &dev->bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i80 bus init failed: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    if (ops->init) {
        ret = ops->init(dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(ret));
            epd_i80_bus_deinit(dev->bus);
            free(dev);
            return ret;
        }
    }

    ESP_LOGI(TAG, "panel created  %ux%u  %d-bit bus",
             config->width, config->height, config->bus_width);
    *handle = dev;
    return ESP_OK;
}

esp_err_t epd_panel_destroy(epd_panel_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL handle");

    if (handle->ops.deinit) {
        handle->ops.deinit(handle);
    }
    epd_i80_bus_deinit(handle->bus);
    free(handle);
    return ESP_OK;
}

/* ── Operations ──────────────────────────────────────────────────────────── */

esp_err_t epd_panel_power_on(epd_panel_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL handle");
    return handle->ops.power_on(handle);
}

esp_err_t epd_panel_power_off(epd_panel_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL handle");
    return handle->ops.power_off(handle);
}

esp_err_t epd_panel_clean(epd_panel_handle_t handle, int cycles)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    if (cycles < 1) {
        cycles = 1;
    }

    for (int i = 0; i < cycles; i++) {
        /*
         * Dispatched straight through the ops table rather than through
         * epd_panel_refresh(), which requires a non-NULL next_buf.  INIT drives
         * uniform frames and never reads pixel data, so demanding a framebuffer
         * here would only force callers to invent one.
         */
        esp_err_t ret = handle->ops.refresh(handle, NULL, NULL,
                                            EPD_WAVEFORM_INIT, NULL);
        ESP_RETURN_ON_ERROR(ret, TAG, "clean cycle %d failed", i + 1);

        /*
         * Let the particles settle between cycles.  Back-to-back cycles are
         * less effective than spaced ones: the pigment is still moving when the
         * next drive starts, so it never fully reaches the rail the cycle was
         * driving it to.  This also yields to the idle task, which matters
         * because a cycle is a second of near-solid CPU.
         */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

esp_err_t epd_panel_refresh(epd_panel_handle_t  handle,
                             const void         *prev_buf,
                             const void         *next_buf,
                             epd_waveform_mode_t mode)
{
    return epd_panel_refresh_area(handle, prev_buf, next_buf, mode, NULL);
}

esp_err_t epd_panel_refresh_area(epd_panel_handle_t  handle,
                                  const void         *prev_buf,
                                  const void         *next_buf,
                                  epd_waveform_mode_t mode,
                                  const epd_rect_t   *area)
{
    ESP_RETURN_ON_FALSE(handle && next_buf, ESP_ERR_INVALID_ARG, TAG, "NULL arg");

    if (area) {
        ESP_RETURN_ON_FALSE(area->x <= handle->cfg.width && area->y <= handle->cfg.height,
                            ESP_ERR_INVALID_ARG, TAG, "area origin out of bounds");
        ESP_RETURN_ON_FALSE((uint32_t)area->x + area->w <= handle->cfg.width &&
                            (uint32_t)area->y + area->h <= handle->cfg.height,
                            ESP_ERR_INVALID_ARG, TAG, "area extends past panel");
        if (area->w == 0 || area->h == 0) {
            return ESP_OK;   /* nothing to draw */
        }
    }

    return handle->ops.refresh(handle, prev_buf, next_buf, mode, area);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

epd_i80_bus_handle_t epd_panel_get_bus(epd_panel_handle_t handle)
{
    return handle ? handle->bus : NULL;
}

tps65185_handle_t epd_panel_get_pmic(epd_panel_handle_t handle)
{
    return handle ? handle->cfg.pmic : NULL;
}

const epd_panel_config_t *epd_panel_get_config(epd_panel_handle_t handle)
{
    return handle ? &handle->cfg : NULL;
}

void epd_panel_set_priv(epd_panel_handle_t handle, void *priv)
{
    if (handle) handle->priv = priv;
}

void *epd_panel_get_priv(epd_panel_handle_t handle)
{
    return handle ? handle->priv : NULL;
}
