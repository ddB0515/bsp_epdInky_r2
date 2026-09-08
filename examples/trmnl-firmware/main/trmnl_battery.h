/*
 * Battery status over the STC3115 fuel gauge.
 *
 * There is no VBUS/USB-detect pin anywhere on this board's schematic (checked
 * the whole address/pin ledger in bsp/epdinky_p4_board.h - nothing routed),
 * and the STC3115 itself is always populated (BSP_DEV_REQUIRED in
 * epdinky_p4_board.c, unlike the accelerometer or RTC which are genuinely
 * optional) - so "is a battery plugged in at all" cannot come from a pin and
 * has to come from the gauge's own reading of its VBAT line instead.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "stc3115.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float voltage_v;  /**< Battery voltage in volts. 0 when !present.        */
    bool  present;    /**< False when no battery is detected - see below.    */
    bool  charging;   /**< True while the gauge reports charge current, or a
                            battery it considers full (which on this board
                            only happens while still connected to charge
                            power holding it there).                        */
} trmnl_battery_status_t;

/** @brief  Remember the handle bsp_epdinky_init_with_config() already brought
 *          up. Does not touch the gauge itself - it is already running. */
esp_err_t trmnl_battery_init(stc3115_handle_t handle);

/**
 * @brief  Read the current battery status.
 *
 * "Present" is derived, not measured directly: the gauge's own BATFAIL bit
 * is defined as exactly this condition (UVLO < 2.6 V, upstream's own name for
 * it is "battery removal detected"), backed by a plausibility floor on the
 * voltage in case BATFAIL is ever wrong. Both describe the same "no battery"
 * condition from two angles rather than being two different thresholds.
 *
 * @return ESP_ERR_INVALID_STATE  trmnl_battery_init() was never called
 *         (whatever the underlying esp_err_t from the gauge itself) on an
 *         I2C failure - @p out is zeroed either way, so a caller that treats
 *         a failure as "no reading" rather than checking present explicitly
 *         still behaves reasonably.
 */
esp_err_t trmnl_battery_read(trmnl_battery_status_t *out);

#ifdef __cplusplus
}
#endif
