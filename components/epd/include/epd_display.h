#ifndef EPD_DISPLAY_H
#define EPD_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "epd_panel.h"
#include "epd_panel_def.h"
#include "epd_board.h"
#include "tps65185.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * E Ink parallel EPD panel
 *
 * Source bus:  16-bit, D0–D15 → GPIO 2–17
 * Gate clock:  CKV → GPIO 51    (max 200 kHz per datasheet)
 * Source clk:  CL  → GPIO 50    (LCD i80 WR, DMA-driven, 10 MHz)
 * Control:     SPV → GPIO 45    (gate start pulse)
 *              SPH → GPIO 46    (source start / XSTL)
 *              OE  → GPIO 47    (output enable, active-low)
 *              LE  → GPIO 48    (source latch enable / XLE)
 *
 * PMIC pins (owned by TPS65185 driver, NOT touched here):
 *              PWR_WAKE  → GPIO 26
 *              PWR_GOOD  → GPIO 27
 *
 * Pixel format (framebuffer passed to epd_panel_refresh):
 *   4 bits per pixel, packed two pixels per byte.
 *   High nibble = left pixel, low nibble = right pixel.
 *   Value 0x0 = black, 0xF = white.
 *   Buffer size = (width * height) / 2 bytes.
 *   Allocate from PSRAM: heap_caps_malloc(size, MALLOC_CAP_SPIRAM).
 ******************************************************************************/

/*
 * Panel geometry now comes from the epd_panel_def_t passed to
 * epd_display_panel_create() - see epd_panel_def.h and the catalogue in
 * epd_panels.h.  Use def->width / def->height, or the accessors on the panel
 * handle, rather than compile-time constants.
 */

/**
 * @brief  Create an EPD panel instance for a given panel definition.
 *
 * Initialises the i80 source bus, configures control GPIOs and allocates the
 * DMA row buffers.  Does NOT power on the display - call epd_panel_power_on()
 * separately, then epd_panel_refresh() to draw.
 *
 * @param def      Panel definition: geometry, orientation, VCOM, AC timing
 *                 and waveform model.  Copied into the panel, so it need not
 *                 outlive this call - a definition on the caller's stack is
 *                 fine.
 * @param pmic     Initialised TPS65185 handle.
 * @param handle   Output handle.
 * @return esp_err_t  ESP_OK on success.
 */
esp_err_t epd_display_panel_create(const epd_panel_def_t    *def,
                                   const epd_board_config_t *board,
                                   tps65185_handle_t         pmic,
                                   epd_panel_handle_t       *handle);

#ifdef __cplusplus
}
#endif

#endif /* EPD_DISPLAY_H */
