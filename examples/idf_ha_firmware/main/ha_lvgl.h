/*
 * LVGL bring-up: tick, draw buffers, flush callback, the dedicated LVGL task,
 * and the lock every other task must hold before touching an LVGL object.
 *
 * Adapted from examples/idf_dsi_camera_preview/main/app_ui.c's LVGL plumbing
 * (app_ui_lock()/app_ui_unlock()/the flush+tick+task wiring), with the
 * camera/PPA-specific parts removed - that example shared its frame buffer
 * between the PPA and LVGL by never letting them touch the same pixels; this
 * firmware has no second writer, so the whole frame buffer is LVGL's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start LVGL against the panel from ha_panel.h.
 *
 * Requires ha_panel_init() to have already run. Registers a touch input
 * device via ha_touch.h if ha_touch_init() succeeds - touch is optional, the
 * dashboard still works display-only otherwise. Starts the LVGL task; from
 * this point on, any other task must call ha_lvgl_lock()/ha_lvgl_unlock()
 * around LVGL API calls.
 */
esp_err_t ha_lvgl_init(void);

/** @brief Take the LVGL lock. Pass 0 to wait indefinitely, ms otherwise. */
bool ha_lvgl_lock(uint32_t timeout_ms);

/** @brief Release the LVGL lock. */
void ha_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
