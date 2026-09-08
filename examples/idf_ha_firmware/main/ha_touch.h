/*
 * Touch abstraction, same shape as ha_panel.h: every concrete touch driver
 * (today: ha_touch_gt911.c) implements exactly this interface, so a future
 * different touch controller is a new ha_touch_<name>.c with no changes
 * needed in ha_lvgl.c or anywhere else.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up the touch controller.
 *
 * Requires bsp_i2c_init() (or any bsp_*_init()) to already have run.
 *
 * Touch is optional: returning ESP_ERR_NOT_FOUND (the controller didn't
 * answer on I2C) is not fatal anywhere that calls this - the dashboard is
 * still useful display-only.
 */
esp_err_t ha_touch_init(void);

/**
 * @brief  LVGL input-device read callback.
 *
 * Registered with lv_indev_set_read_cb() by ha_lvgl.c only if ha_touch_init()
 * succeeded.
 */
void ha_touch_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
