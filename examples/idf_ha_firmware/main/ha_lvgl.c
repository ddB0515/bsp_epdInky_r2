#include "ha_lvgl.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "ha_panel.h"
#include "ha_touch.h"

static const char *TAG = "ha_lvgl";

/* Small "band" draw buffers, PARTIAL render mode - the configuration
 * examples/idf_dsi_camera_preview already validated on this exact panel. */
#define LVGL_DRAW_LINES 64
#define LVGL_TICK_MS     5
#define LVGL_TASK_STACK  8192

static lv_display_t     *s_disp;
static SemaphoreHandle_t s_lock;

bool ha_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_lock) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void ha_lvgl_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

    /* Hand the rendered block to the DPI and return without calling
     * lv_display_flush_ready(): the transfer is asynchronous, so LVGL must
     * not reuse the buffer until on_trans_done() fires. */
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

static bool on_trans_done(esp_lcd_panel_handle_t panel,
                          esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t wait_ms = 10;
        if (ha_lvgl_lock(0)) {
            wait_ms = lv_timer_handler();
            ha_lvgl_unlock();
        }
        if (wait_ms > 100) {
            wait_ms = 100;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms < 5 ? 5 : wait_ms));
    }
}

esp_err_t ha_lvgl_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "could not create the LVGL mutex");

    lv_init();

    const uint16_t w = ha_panel_width();
    const uint16_t h = ha_panel_height();

    s_disp = lv_display_create(w, h);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "could not create the LVGL display");

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_user_data(s_disp, ha_panel_get_handle());
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* PARTIAL mode with two PSRAM draw buffers: LVGL renders a block, the DPI
     * copies it in asynchronously, and LVGL draws the next block into the
     * other buffer meanwhile. */
    size_t bpp       = lv_color_format_get_size(lv_display_get_color_format(s_disp));
    size_t buf_bytes = (size_t)w * LVGL_DRAW_LINES * bpp;

    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG,
                        "no memory for the LVGL draw buffers");
    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_trans_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_register_event_callbacks(ha_panel_get_handle(), &cbs, s_disp),
        TAG, "could not register the DPI flush-ready callback");

    /* Touch is optional - the dashboard is still useful display-only. */
    esp_err_t touch_err = ha_touch_init();
    if (touch_err == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev, s_disp);
        lv_indev_set_read_cb(indev, ha_touch_indev_read_cb);
        ESP_LOGI(TAG, "touch input ready");
    } else {
        ESP_LOGW(TAG, "no touch controller found (%s) - display-only",
                 esp_err_to_name(touch_err));
    }

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG,
                        "tick start failed");

    BaseType_t ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, 4, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "could not start the LVGL task");

    ESP_LOGI(TAG, "LVGL ready (%dx%d RGB888)", w, h);
    return ESP_OK;
}
