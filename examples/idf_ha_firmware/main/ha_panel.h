/*
 * Panel abstraction.
 *
 * Every concrete panel driver (today: ha_panel_jd9168.c, selected by the
 * Kconfig HA_PANEL choice) implements exactly this interface. Nothing else
 * in this firmware - ha_lvgl.c, ha_dashboard.c, main.c - ever references a
 * panel driver or its vendor headers directly, so a future different panel
 * is a new ha_panel_<name>.c implementing these five functions, with no
 * changes needed anywhere else.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up the panel, its power sequencing and its backlight.
 *
 * Requires bsp_i2c_init() (or any bsp_*_init()) to already have run - the
 * panel's power/reset/backlight-enable expander and the backlight controller
 * itself are both on the shared board I2C bus. The backlight is left off;
 * call ha_panel_set_backlight() once there is something worth showing.
 */
esp_err_t ha_panel_init(void);

/** @brief The underlying esp_lcd panel handle, for ha_lvgl.c to attach to. */
esp_lcd_panel_handle_t ha_panel_get_handle(void);

/** @brief Panel width in pixels. */
uint16_t ha_panel_width(void);

/** @brief Panel height in pixels. */
uint16_t ha_panel_height(void);

/** @brief Set the backlight, 0 (off) to 100 (full) percent. */
esp_err_t ha_panel_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
