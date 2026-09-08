#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RGB888. Not negotiable on this panel.
 *
 * RGB565 would cut the PPA time by 16% and halve the DSI scan-out load, but
 * the JD9168 shows a blank screen with it: the DPI reports a healthy 54 Hz of
 * VSYNC and scan-out is clearly running, yet nothing reaches the glass. The
 * vendor init sequence configures the panel for 24-bit pixels and there is no
 * documented 16-bit variant, so the bandwidth saving is simply not available
 * here. Tested twice, once at each DSI lane rate.
 */
#define APP_DISPLAY_BPP 24
#define APP_DISPLAY_BYTES_PER_PX (APP_DISPLAY_BPP / 8)

/*
 * Screen layout.
 *
 * A title strip across the top, then the camera preview inset below it with a
 * side panel on the right for the resolution buttons. The preview keeps the
 * camera's 16:9 shape.
 *
 *   +--------------------------------------------------+ 0
 *   |  DSI + SC2336 Camera                             | title, 72 px
 *   +-----------------------------------------+--------+ 72
 *   |                                         | 640x480|
 *   |          preview 800x450 (16:9)         |        |
 *   |                                         |1280x720|
 *   +-----------------------------------------+--------+
 *
 * The PPA owns the preview rectangle and LVGL owns everything else. They never
 * overlap, which is what lets both write to the same frame buffer without
 * locking or tearing.
 */
#define APP_DISPLAY_TITLE_H  72
#define APP_DISPLAY_SIDE_W  200

#define APP_DISPLAY_VIDEO_W 800
#define APP_DISPLAY_VIDEO_H 450   /* 800 x 9/16 */
#define APP_DISPLAY_VIDEO_X  12
#define APP_DISPLAY_VIDEO_Y (APP_DISPLAY_TITLE_H + 24)

/**
 * @brief Bring up the MIPI-DSI panel and its backlight.
 *
 * The backlight is on the display module's I2C bus, so bsp_i2c_init() (or any
 * bsp_*_init) must have run first. The backlight is left off; call
 * app_display_set_backlight() once there is something worth showing.
 */
esp_err_t app_display_init(void);

/** @brief The DPI panel handle, for LVGL to attach to. */
esp_lcd_panel_handle_t app_display_panel(void);

/** @brief The frame buffer the panel scans out of. */
void *app_display_frame_buffer(void);

/** @brief Size of the frame buffer in bytes. */
size_t app_display_frame_buffer_size(void);

/** @brief Set the backlight, 0 to 100 percent. */
esp_err_t app_display_set_backlight(uint8_t percent);

/**
 * @brief Show the DSI test pattern.
 *
 * Useful as a first bring-up step: colour bars mean the DSI link, the panel
 * and the backlight are all working, independently of anything the
 * application draws.
 */
esp_err_t app_display_test_pattern(bool on);

/**
 * @brief Fill the whole frame buffer with one colour.
 *
 * A bring-up aid: the DSI test pattern proves the link and the panel, while
 * this proves the frame buffer path on top of it. If the bars appear but a
 * fill does not, the problem is between the CPU and the frame buffer rather
 * than in the display itself.
 */
void app_display_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Write CPU-modified frame buffer bytes back out of the cache.
 *
 * The DPI frame buffer is PSRAM scanned out by DMA, so anything written
 * directly by the CPU must be flushed before the panel can see it. Callers
 * that go through esp_lcd_panel_draw_bitmap() do not need this.
 */
void app_display_sync_cache(void *addr, size_t size);

/**
 * @brief Check that the DPI actually transfers pixels.
 *
 * Pushes a stripe through draw_bitmap() and waits for the completion
 * interrupt, so a stalled DMA is reported in the log rather than having to be
 * spotted by eye. The DSI colour bars cannot detect this: they are generated
 * in the host and never touch the frame buffer or the DMA.
 */
esp_err_t app_display_selftest(void);

/**
 * @brief Fill a rectangle of the frame buffer with one colour.
 *
 * Exercises the same partial-write path that LVGL and the PPA use, so if a
 * full-screen fill appears but a rectangle does not, the fault is in the
 * per-row addressing rather than the display itself.
 */
void app_display_fill_rect(int x, int y, int w, int h,
                           uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
