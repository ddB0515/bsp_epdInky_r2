#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Backlight level the UI starts at, in percent. */
#define APP_UI_BRIGHTNESS_DEFAULT 50

/** @brief One selectable sensor mode shown on the side panel. */
typedef struct {
	uint32_t    w;
	uint32_t    h;
	const char *label;
} app_ui_res_t;

/**
 * @brief Start LVGL and build the preview UI.
 *
 * Must be called after app_display_init(). Touch is optional: if the GT967
 * does not answer, the UI still runs, just without input.
 */
esp_err_t app_ui_init(void);

/** @brief Update the frame-rate readout. Safe to call from the preview task. */
void app_ui_set_fps(float fps);

/** @brief Update the exposure readout. */
void app_ui_set_luma(int32_t luma);

/** @brief Ask for a different sensor mode. Applied by the preview task. */
void app_ui_request_resolution(uint32_t w, uint32_t h);

/**
 * @brief Collect a pending resolution request, if there is one.
 *
 * Called by the preview task between frames: switching sensor mode tears the
 * capture down and rebuilds it, which must not happen mid-frame.
 */
bool app_ui_take_resolution_request(uint32_t *w, uint32_t *h);

/** @brief Ask for a still to be written to the SD card. */
void app_ui_request_save(void);

/**
 * @brief Collect a pending save request, if there is one.
 *
 * Called by the preview task, which already owns the camera; encoding a JPEG
 * from another task would fight it for the capture queue.
 */
bool app_ui_take_save_request(void);

/** @brief Show a short message next to the save button. */
void app_ui_set_status(const char *text, bool ok);

/** @brief Take the LVGL lock. Pass 0 to wait indefinitely. */
bool app_ui_lock(uint32_t timeout_ms);

/** @brief Release the LVGL lock. */
void app_ui_unlock(void);

#ifdef __cplusplus
}
#endif
