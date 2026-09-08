/*
 * The LVGL-facing half of the Home Assistant integration: builds one tile per
 * entity in ha_dashboard_config.h, keeps them in sync with ha_ws.c's entity
 * cache, and turns a tap into a ha_ws_call_service() call.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Build the dashboard UI.
 *
 * Requires ha_lvgl_init() to have already run. Call ha_ws_init() first (no
 * networking happens there, just setup) so this can do an initial
 * ha_ws_get_state() pass; call ha_ws_start() only after this returns, so the
 * WebSocket task's callbacks never fire before this module has registered
 * for them.
 *
 * Every tile starts in a dimmed "connecting..." placeholder state until
 * either that initial pass finds cached data or the first real update
 * arrives from ha_ws.c.
 */
esp_err_t ha_dashboard_init(void);

/**
 * @brief  Wake the backlight to full brightness immediately, as if a touch
 *         had just happened, without needing a real touch event.
 *
 * Used by main.c for the button's short-press gesture.
 */
void ha_dashboard_wake_backlight(void);

#ifdef __cplusplus
}
#endif
