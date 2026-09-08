/*
 * Per-core CPU load, for the on-screen stats overlay.
 *
 * LVGL has its own performance monitor (LV_USE_PERF_MONITOR), but its "CPU"
 * figure comes from lv_timer_get_idle(), which measures how busy the LVGL
 * timer handler is - not the chip. In this example the PPA draws the video and
 * LVGL only repaints a few labels, so the built-in monitor would sit near 0%
 * while both cores were genuinely working. These numbers come from the
 * FreeRTOS idle-task run-time counters instead, so they reflect the whole
 * system.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of cores reported by app_stats_cpu(). */
#define APP_STATS_CORES 2

/**
 * @brief Start sampling CPU load.
 *
 * Takes the first reading, so the figure returned by the next call covers the
 * interval since this one.
 */
esp_err_t app_stats_init(void);

/**
 * @brief Busy percentage of each core since the previous call.
 *
 * Measures the interval between calls, so it should be called on a steady
 * period. Returns ESP_ERR_INVALID_STATE if the interval was too short to be
 * meaningful.
 *
 * @param busy  Receives one percentage per core, 0..100.
 */
esp_err_t app_stats_cpu(float busy[APP_STATS_CORES]);

#ifdef __cplusplus
}
#endif
