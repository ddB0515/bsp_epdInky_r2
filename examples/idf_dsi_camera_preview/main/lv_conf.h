/*
 * Minimal LVGL configuration for the camera preview.
 *
 * RGB888 to match the panel's frame buffer, and PSRAM for LVGL's own pool -
 * internal RAM is needed by the camera and DSI drivers.
 */
#pragma once

#define LV_CONF_SKIP 0

/*
 * 16, even though the panel is RGB888.
 *
 * This only sets LVGL's default; the display is told to render RGB888 with
 * lv_display_set_color_format(). LVGL's software renderer is written around
 * 16- and 32-bit depths, and setting 24 here produces a blank screen on this
 * panel. The known-good vendor demo uses the same 16 + explicit RGB888 pairing.
 */
#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_TICK_CUSTOM 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_LOG 0
