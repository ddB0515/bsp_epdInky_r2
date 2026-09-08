#include "epd_i80_bus.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "epd_i80_bus";

struct epd_i80_bus_dev {
    esp_lcd_i80_bus_handle_t  lcd_bus;
    esp_lcd_panel_io_handle_t lcd_io;
    epd_i80_bus_config_t      cfg;
    SemaphoreHandle_t         trans_done_sem;
    SemaphoreHandle_t         tx_mutex;   /**< serialises epd_i80_bus_send_row */
    bool                      tx_pending; /**< a DMA is in flight (mutex held) */
    volatile int64_t          done_us;    /**< esp_timer time of last ISR done */
    portMUX_TYPE              spinlock;   /**< guards bit-banged pulse trains  */
};

/* ── DMA completion callback ──────────────────────────────────────────────── */

static bool IRAM_ATTR trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    struct epd_i80_bus_dev *dev = (struct epd_i80_bus_dev *)user_ctx;
    /* Timestamp the hardware completion itself, before any task gets to run,
     * so callers can separate real DMA time from scheduler wake latency. */
    dev->done_us = esp_timer_get_time();
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(dev->trans_done_sem, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;
}

int64_t epd_i80_bus_last_done_us(epd_i80_bus_handle_t handle)
{
    return handle ? handle->done_us : 0;
}

/* ── Init / deinit ────────────────────────────────────────────────────────── */

esp_err_t epd_i80_bus_init(const epd_i80_bus_config_t *config,
                            uint8_t bus_width,
                            epd_i80_bus_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && handle, ESP_ERR_INVALID_ARG, TAG, "NULL arg");
    ESP_RETURN_ON_FALSE(bus_width == 8 || bus_width == 16,
                        ESP_ERR_INVALID_ARG, TAG, "bus_width must be 8 or 16");
    ESP_RETURN_ON_FALSE(config->max_row_bytes > 0,
                        ESP_ERR_INVALID_ARG, TAG, "max_row_bytes must be non-zero");
    ESP_RETURN_ON_FALSE(config->pclk_hz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "pclk_hz must be non-zero");

    struct epd_i80_bus_dev *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "alloc failed");

    esp_err_t ret = ESP_OK;  /* required by ESP_GOTO_ON_ERROR */
    dev->cfg = *config;
    portMUX_INITIALIZE(&dev->spinlock);

    dev->trans_done_sem = xSemaphoreCreateBinary();
    dev->tx_mutex       = xSemaphoreCreateMutex();
    if (!dev->trans_done_sem || !dev->tx_mutex) {
        if (dev->trans_done_sem) vSemaphoreDelete(dev->trans_done_sem);
        if (dev->tx_mutex)       vSemaphoreDelete(dev->tx_mutex);
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    /* ── Configure bit-bang GPIO outputs ──────────────────────────────────── */
    /*
     * OE, LE, SPV, CKV and GMOD are pure GPIO outputs.
     * CL (pin_cl) is handled by the LCD peripheral below.
     * The TPS65185 pins (WAKEUP / PWRUP / VCOM_CTRL / PWR_GOOD) are owned by
     * the PMIC driver and must not be touched here.
     *
     * SPH (pin_sph) is intentionally excluded: it is assigned to the LCD
     * i80 peripheral as hardware CS below.  The peripheral takes ownership
     * of the pin and drives it low/high automatically around each transfer.
     */
    const gpio_num_t ctrl_pins[] = {
        config->pin_le,
        config->pin_oe,
        config->pin_spv,
        config->pin_ckv,
        config->pin_gmod,
    };

    for (int i = 0; i < (int)(sizeof(ctrl_pins) / sizeof(ctrl_pins[0])); i++) {
        if (ctrl_pins[i] == GPIO_NUM_NC) continue;
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << ctrl_pins[i],
            .mode         = GPIO_MODE_OUTPUT,
            .intr_type    = GPIO_INTR_DISABLE,
            .pull_up_en   = 0,
            .pull_down_en = 0,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&gc), fail_gpio, TAG,
                          "GPIO %d config failed", ctrl_pins[i]);
    }

    /* Safe idle levels (skip pins the board does not wire up) */
    if (config->pin_oe   != GPIO_NUM_NC) {
        /* Start with outputs disabled until the panel driver powers up. */
        gpio_set_level(config->pin_oe, config->oe_active_high ? 0 : 1);
    }
    if (config->pin_le   != GPIO_NUM_NC) gpio_set_level(config->pin_le,   0);  /* LE idle low   */
    if (config->pin_spv  != GPIO_NUM_NC) gpio_set_level(config->pin_spv,  1);  /* SPV idle high */
    if (config->pin_ckv  != GPIO_NUM_NC) gpio_set_level(config->pin_ckv,  0);  /* CKV idle low  */
    if (config->pin_gmod != GPIO_NUM_NC) gpio_set_level(config->pin_gmod, 0);  /* gate driver off */

    /* ── Create LCD i80 bus ───────────────────────────────────────────────── */
    /*
     * dc_gpio_num: EPD panels have no command/data distinction on the source
     * bus. GPIO_NUM_NC disables DC pin configuration; the driver only calls
     * tx_color so no DC toggling occurs.
     *
     * cs_gpio_num: EPD panels have no CS on the source bus.
     */
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num        = config->pin_dc_dummy,  /* required by IDF v6.0; EPD has no DC line */
        .wr_gpio_num        = config->pin_cl,
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .bus_width          = bus_width,
        .max_transfer_bytes = config->max_row_bytes,
        .dma_burst_size     = 64,   /* ESP32-P4 DMA burst / cache-line size */
    };

    for (int i = 0; i < bus_width; i++) {
        bus_cfg.data_gpio_nums[i] = config->data_pins[i];
    }
    /* Unused upper half in 8-bit mode */
    for (int i = bus_width; i < 16; i++) {
        bus_cfg.data_gpio_nums[i] = GPIO_NUM_NC;
    }

    ESP_GOTO_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &dev->lcd_bus),
                      fail_gpio, TAG, "esp_lcd_new_i80_bus failed");

    /* ── Create panel IO on that bus ─────────────────────────────────────── */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        /*
         * SPH is the source-start pulse of the EPD.  Driven by hardware CS:
         * goes LOW at the start of each pixel transfer, HIGH at the end.
         */
        .cs_gpio_num           = config->pin_sph,
        .pclk_hz               = config->pclk_hz,
        .trans_queue_depth     = 4,
        .on_color_trans_done   = trans_done_cb,
        .user_ctx              = dev,
        .lcd_cmd_bits   = 0,   /* No command phase */
        .lcd_param_bits = 0,
        .dc_levels = {
            .dc_idle_level  = 1,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 1,
            .dc_data_level  = 1,
        },
        .flags = {
            .cs_active_high  = 0,
            .pclk_active_neg = 0,
            .pclk_idle_low   = 0,
        },
    };

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i80(dev->lcd_bus, &io_cfg, &dev->lcd_io),
                      fail_io, TAG, "esp_lcd_new_panel_io_i80 failed");

    *handle = dev;
    ESP_LOGI(TAG, "init OK  bus_width=%d  pclk=%lu Hz", bus_width,
             (unsigned long)config->pclk_hz);
    return ESP_OK;

fail_io:
    esp_lcd_del_i80_bus(dev->lcd_bus);
fail_gpio:
    vSemaphoreDelete(dev->trans_done_sem);
    vSemaphoreDelete(dev->tx_mutex);
    free(dev);
    return ret;   /* propagate the real cause, not a generic ESP_FAIL */
}

esp_err_t epd_i80_bus_deinit(epd_i80_bus_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL handle");
    /* Never tear the peripheral down underneath an in-flight DMA. */
    epd_i80_bus_wait_row(handle);
    esp_lcd_panel_io_del(handle->lcd_io);
    esp_lcd_del_i80_bus(handle->lcd_bus);
    vSemaphoreDelete(handle->trans_done_sem);
    vSemaphoreDelete(handle->tx_mutex);
    free(handle);
    return ESP_OK;
}

/* ── Row transfer ─────────────────────────────────────────────────────────── */

/*
 * Upper bound for a single row DMA.  A full row is only a few tens of
 * microseconds at any sane pclk, so this is purely a deadlock guard: without
 * it a lost completion interrupt would wedge the refresh task forever.
 */
#define EPD_I80_ROW_TIMEOUT_MS  100

esp_err_t epd_i80_bus_send_row_async(epd_i80_bus_handle_t handle,
                                      const void *row_data,
                                      size_t len_bytes)
{
    ESP_RETURN_ON_FALSE(handle && row_data && len_bytes,
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /*
     * Catch a submit that was not paired with a wait.  Taking the
     * non-recursive mutex twice from the same task would otherwise self-
     * deadlock with no diagnostic.
     */
    ESP_RETURN_ON_FALSE(!handle->tx_pending, ESP_ERR_INVALID_STATE, TAG,
                        "transfer already in flight");

    if (xSemaphoreTake(handle->tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /*
     * lcd_cmd = 0 with lcd_cmd_bits = 0 → no command prefix, pure data.
     * Drain any stale give left behind by a previously timed-out transfer so
     * we cannot mistake it for this transfer's completion.
     */
    xSemaphoreTake(handle->trans_done_sem, 0);

    esp_err_t ret = esp_lcd_panel_io_tx_color(handle->lcd_io, 0, row_data, len_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tx_color failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(handle->tx_mutex);
        return ret;
    }

    handle->tx_pending = true;
    return ESP_OK;
}

esp_err_t epd_i80_bus_wait_row(epd_i80_bus_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "NULL handle");

    if (!handle->tx_pending) {
        return ESP_OK;   /* nothing outstanding — safe to call as a drain */
    }

    /*
     * Only the task that submitted may wait, because completing the wait gives
     * back tx_mutex.  Giving a FreeRTOS mutex from a task that does not hold it
     * trips an assert in xTaskPriorityDisinherit() (or, with asserts compiled
     * out, silently corrupts the real owner's priority-inheritance state), so
     * refuse rather than proceed.
     */
    if (xSemaphoreGetMutexHolder(handle->tx_mutex) != xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "wait_row from a task that did not submit");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (xSemaphoreTake(handle->trans_done_sem,
                       pdMS_TO_TICKS(EPD_I80_ROW_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "row DMA timed out after %d ms", EPD_I80_ROW_TIMEOUT_MS);
        ret = ESP_ERR_TIMEOUT;
    }

    handle->tx_pending = false;
    xSemaphoreGive(handle->tx_mutex);
    return ret;
}

esp_err_t epd_i80_bus_send_row(epd_i80_bus_handle_t handle,
                                const void *row_data,
                                size_t len_bytes)
{
    esp_err_t ret = epd_i80_bus_send_row_async(handle, row_data, len_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    return epd_i80_bus_wait_row(handle);
}

/* ── Gate / control signal primitives ────────────────────────────────────── */

void epd_i80_ckv_high(epd_i80_bus_handle_t h)    { gpio_set_level(h->cfg.pin_ckv, 1); }
void epd_i80_ckv_low(epd_i80_bus_handle_t h)     { gpio_set_level(h->cfg.pin_ckv, 0); }
void epd_i80_spv_high(epd_i80_bus_handle_t h)    { gpio_set_level(h->cfg.pin_spv, 1); }
void epd_i80_spv_low(epd_i80_bus_handle_t h)     { gpio_set_level(h->cfg.pin_spv, 0); }
void epd_i80_le_high(epd_i80_bus_handle_t h)     { gpio_set_level(h->cfg.pin_le,  1); }
void epd_i80_le_low(epd_i80_bus_handle_t h)      { gpio_set_level(h->cfg.pin_le,  0); }

void epd_i80_oe_enable(epd_i80_bus_handle_t h)
{
    if (h->cfg.pin_oe == GPIO_NUM_NC) return;
    gpio_set_level(h->cfg.pin_oe, h->cfg.oe_active_high ? 1 : 0);
}

void epd_i80_oe_disable(epd_i80_bus_handle_t h)
{
    if (h->cfg.pin_oe == GPIO_NUM_NC) return;
    gpio_set_level(h->cfg.pin_oe, h->cfg.oe_active_high ? 0 : 1);
}

void epd_i80_gmod_set(epd_i80_bus_handle_t h, bool enable)
{
    if (h->cfg.pin_gmod == GPIO_NUM_NC) return;
    gpio_set_level(h->cfg.pin_gmod, enable ? 1 : 0);
}

/* ── Timing-critical section ─────────────────────────────────────────────── */

void epd_i80_bus_enter_critical(epd_i80_bus_handle_t h)
{
    portENTER_CRITICAL(&h->spinlock);
}

void epd_i80_bus_exit_critical(epd_i80_bus_handle_t h)
{
    portEXIT_CRITICAL(&h->spinlock);
}
