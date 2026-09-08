/*
 * LVGL to E Ink bridge. See app_epd_lvgl.h for the design.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "epd_fb.h"

#include "app_epd_lvgl.h"

static const char *TAG = "epd_lvgl";

/* Rows per LVGL draw band. 40 x 1872 is 75 KB per buffer in internal RAM. */
#define LVGL_DRAW_LINES 40

static app_epd_lvgl_config_t s_cfg;
static lv_display_t         *s_disp;
static SemaphoreHandle_t     s_lock;

/* LVGL's render target: one byte of grey per pixel. */
static uint8_t *s_l8;
static uint32_t  s_w, s_h;

/* What finally goes to the panel: two pixels per byte. */
static epd_fb_t s_fb;

/* Set by the flush callback, cleared once the change reaches the glass. */
static volatile bool     s_dirty;
static volatile int64_t  s_first_dirty_us;   /* start of the current dirty run */
static volatile int64_t  s_last_dirty_us;    /* most recent change             */

/*
 * On-demand requests, set from LVGL event callbacks.
 *
 * An event callback runs inside lv_timer_handler() with the LVGL lock held.
 * Refreshing there would block LVGL for over a second mid-dispatch, so the
 * callback only raises a flag and the policy loop acts on it once the handler
 * has returned.
 */
static volatile bool     s_want_refresh;
static volatile bool     s_want_clean;

/*
 * L8 to 4bpp.
 *
 * Both are greyscale and both run 0 = black to max = white, so this is a shift
 * rather than a conversion: the top nibble of each byte is the pixel. Two
 * source bytes pack into one destination byte, high nibble first, matching
 * epd_fb's layout.
 */
static void l8_to_4bpp(void)
{
    const uint32_t px  = s_w * s_h;
    const uint8_t *src = s_l8;
    uint8_t       *dst = s_fb.buf;

    for (uint32_t i = 0; i < px; i += 2) {
        dst[i >> 1] = (uint8_t)((src[i] & 0xF0u) | (src[i + 1] >> 4));
    }
}

/*
 * Rails are up only while an update is actually running.
 *
 * Leaving them up between updates is what makes white go grainy. With the
 * rails on, VCOM sits live at its operating voltage while the source lines
 * rest near ground, so every pixel on the panel sees a DC field of the full
 * VCOM magnitude continuously - the same bias epd_display_power_off() goes to
 * such lengths to neutralise, only sustained for minutes instead of one scan.
 * The pigment drifts under it, and a uniform white area is where that shows
 * first.
 *
 * The refcount means a clean followed by its redraw is one power cycle rather
 * than two.
 */
static bool s_rails_up;

static esp_err_t rails_up(void)
{
    if (s_rails_up) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(epd_panel_power_on(s_cfg.panel), TAG, "power on failed");
    s_rails_up = true;
    return ESP_OK;
}

static void rails_down(void)
{
    if (!s_rails_up || s_cfg.keep_rails_on) {
        return;
    }
    esp_err_t ret = epd_panel_power_off(s_cfg.panel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "power off failed: %s", esp_err_to_name(ret));
    }
    s_rails_up = false;
}

static esp_err_t push_to_panel(app_epd_lvgl_mode_t mode)
{
    l8_to_4bpp();

    int64_t t0 = esp_timer_get_time();

    ESP_RETURN_ON_ERROR(rails_up(), TAG, "rails up failed");

    esp_err_t ret = ESP_OK;
    if (mode == APP_EPD_LVGL_FULL) {
        ret = epd_panel_refresh(s_cfg.panel, NULL, s_fb.buf, EPD_WAVEFORM_INIT);
    }
    if (ret == ESP_OK) {
        ret = epd_panel_refresh(s_cfg.panel, NULL, s_fb.buf, EPD_WAVEFORM_GC16);
    }

    /* Drop the rails even on failure: a half-updated panel left biased is
     * worse than a half-updated panel left neutral. */
    rails_down();

    ESP_RETURN_ON_ERROR(ret, TAG, "refresh failed");

    ESP_LOGI(TAG, "refresh (%s) in %lld ms",
             mode == APP_EPD_LVGL_FULL ? "full" : "gc16",
             (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;
}

/*
 * LVGL flush.
 *
 * PARTIAL render mode, so px_map holds just the rectangle LVGL redrew. It is
 * copied into the full-screen shadow buffer and the change is recorded; the
 * panel is not touched here.
 *
 * Blocking for a 1.4 s refresh inside this callback would stall LVGL's whole
 * pipeline, so the policy in the LVGL task decides when to pay for the glass.
 * The shadow buffer is what makes that possible: it always holds the complete
 * screen, so a refresh can fire at any moment and still send a whole image.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t x1 = area->x1, x2 = area->x2;
    const int32_t y1 = area->y1, y2 = area->y2;
    const int32_t w  = x2 - x1 + 1;

    /* One byte per pixel in both buffers, so each row is a straight copy. */
    for (int32_t y = y1; y <= y2; y++) {
        memcpy(s_l8 + (size_t)y * s_w + x1, px_map, (size_t)w);
        px_map += w;
    }

    int64_t now = esp_timer_get_time();
    if (!s_dirty) {
        s_first_dirty_us = now;
    }
    s_last_dirty_us = now;
    s_dirty = true;

    lv_display_flush_ready(disp);
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

esp_err_t app_epd_lvgl_init(const app_epd_lvgl_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->panel, ESP_ERR_INVALID_ARG, TAG, "bad config");

    s_cfg = *cfg;
    if (s_cfg.settle_ms == 0) {
        s_cfg.settle_ms = 400;
    }

    const epd_panel_config_t *pc = epd_panel_get_config(cfg->panel);
    s_w = pc->width;
    s_h = pc->height;

    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mutex");

    ESP_RETURN_ON_ERROR(epd_fb_create(&s_fb, (uint16_t)s_w, (uint16_t)s_h),
                        TAG, "framebuffer allocation failed");
    epd_fb_fill(&s_fb, 0xF);

    /*
     * One byte per pixel: 2.6 MB at 1872x1404, so PSRAM. Internal RAM could
     * not hold it and does not need to - nothing here is DMA'd, since the
     * conversion to 4bpp is done by the CPU.
     */
    const size_t l8_bytes = (size_t)s_w * s_h;
    s_l8 = heap_caps_malloc(l8_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_l8, ESP_ERR_NO_MEM, TAG,
                        "no PSRAM for the %u KB shadow buffer",
                        (unsigned)(l8_bytes / 1024));
    memset(s_l8, 0xFF, l8_bytes);        /* white */

    lv_init();

    s_disp = lv_display_create((int32_t)s_w, (int32_t)s_h);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display create failed");

    /*
     * L8 and DIRECT mode.
     *
     * L8 is 8-bit greyscale, which is what the panel wants minus four bits -
     * rendering in RGB565 would mean a colour conversion and a tone mapping
     * pass on every flush for no benefit.
     *
     * DIRECT mode means LVGL renders straight into this buffer and keeps its
     * contents between frames, so the buffer always holds the whole screen.
     * That is what lets the refresh policy fire at any moment and still send a
     * complete image.
     */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_L8);

    /*
     * PARTIAL mode with two small draw buffers.
     *
     * LVGL renders a band at a time and the flush copies it into the shadow.
     * DIRECT mode - handing LVGL the shadow buffer itself - looks simpler and
     * removes the copy, but LVGL 9.5 hangs inside lv_timer_handler() the
     * second time round with an L8 single-buffer direct display. PARTIAL is
     * the conventional path and is what the library is exercised against.
     *
     * The bands are small enough to sit in internal RAM, which the software
     * renderer touches far more than it touches the shadow.
     */
    const size_t band_bytes = (size_t)s_w * LVGL_DRAW_LINES;
    void *b1 = heap_caps_malloc(band_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *b2 = heap_caps_malloc(band_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(b1 && b2, ESP_ERR_NO_MEM, TAG, "no memory for draw buffers");
    lv_display_set_buffers(s_disp, b1, b2, band_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name     = "lv_tick",
    };
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick create");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, 1000), TAG, "tick start");

    ESP_LOGI(TAG, "LVGL ready: %ux%u L8, shadow %u KB, settle %u ms",
             (unsigned)s_w, (unsigned)s_h, (unsigned)(l8_bytes / 1024),
             (unsigned)s_cfg.settle_ms);
    return ESP_OK;
}

bool app_epd_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_lock) {
        return false;
    }
    TickType_t t = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE;
}

void app_epd_lvgl_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

lv_display_t *app_epd_lvgl_display(void)
{
    return s_disp;
}

esp_err_t app_epd_lvgl_refresh_now(app_epd_lvgl_mode_t mode)
{
    if (!app_epd_lvgl_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    /* Let LVGL finish anything outstanding, so the buffer is complete. */
    lv_refr_now(s_disp);
    s_dirty = false;
    esp_err_t ret = push_to_panel(mode);
    app_epd_lvgl_unlock();
    return ret;
}

esp_err_t app_epd_lvgl_clean(int cycles)
{
    /* Rails stay up across both, so the clean and its redraw are one cycle;
     * push_to_panel() drops them at the end. */
    ESP_RETURN_ON_ERROR(rails_up(), TAG, "rails up failed");
    esp_err_t ret = epd_panel_clean(s_cfg.panel, cycles);
    if (ret != ESP_OK) {
        rails_down();
        ESP_RETURN_ON_ERROR(ret, TAG, "clean failed");
    }
    return app_epd_lvgl_refresh_now(APP_EPD_LVGL_FULL);
}

void app_epd_lvgl_request_refresh(void)
{
    s_want_refresh = true;
}

void app_epd_lvgl_request_clean(void)
{
    s_want_clean = true;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    app_epd_lvgl_run();
}

/*
 * LVGL's timers and the refresh policy, interleaved on one task.
 *
 * Both live here so the panel has a single owner. The EPD bus serialises
 * transfers with a mutex taken with portMAX_DELAY, so refreshing from two
 * tasks at once would block one of them with nothing in the log.
 *
 * The ordering matters: run LVGL's timers first so the shadow buffer reflects
 * this instant, then decide whether to pay for the glass.
 */
void app_epd_lvgl_run(void)
{
    if (s_cfg.startup_clean_cycles > 0) {
        ESP_LOGI(TAG, "deep clean, %d cycles", s_cfg.startup_clean_cycles);
        app_epd_lvgl_clean(s_cfg.startup_clean_cycles);
    } else {
        app_epd_lvgl_refresh_now(APP_EPD_LVGL_FULL);
    }
    ESP_LOGI(TAG, "first screen shown");

    while (true) {
        uint32_t wait_ms = 5;
        if (app_epd_lvgl_lock(0)) {
            wait_ms = lv_timer_handler();
            app_epd_lvgl_unlock();
        }
        /*
         * Cap the sleep. LVGL returns how long until its next timer, which
         * with a static screen is effectively forever, but the refresh policy
         * below still needs to notice a settle window expiring.
         */
        if (wait_ms > 20) {
            wait_ms = 20;
        }

        if (s_want_clean) {
            s_want_clean  = false;
            s_want_refresh = false;
            app_epd_lvgl_clean(s_cfg.startup_clean_cycles > 0
                                   ? s_cfg.startup_clean_cycles : 2);
        } else if (s_want_refresh) {
            s_want_refresh = false;
            app_epd_lvgl_refresh_now(APP_EPD_LVGL_FULL);
        } else if (s_dirty) {
            const int64_t now      = esp_timer_get_time();
            const int64_t quiet_ms = (now - s_last_dirty_us) / 1000;
            const int64_t age_ms   = (now - s_first_dirty_us) / 1000;

            const bool settled = quiet_ms >= (int64_t)s_cfg.settle_ms;
            const bool overdue = s_cfg.max_defer_ms &&
                                 age_ms >= (int64_t)s_cfg.max_defer_ms;

            if (settled || overdue) {
                if (app_epd_lvgl_lock(0)) {
                    s_dirty = false;
                    push_to_panel(s_cfg.mode);
                    app_epd_lvgl_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms ? wait_ms : 1));
    }
}

esp_err_t app_epd_lvgl_start(void)
{
    /*
     * 24 KB, well above LVGL's own needs, because a refresh runs the EPD
     * driver's row construction on this stack too. The default task stack
     * overflows here and reports a stack protection fault, which reads like
     * memory corruption rather than a sizing problem.
     *
     * Pinned to core 0, the core the EPD bus and its DMA interrupt belong to,
     * so a refresh and its completion handling stay together.
     */
    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 24 * 1024,
                                            NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}
