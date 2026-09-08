#ifndef EPD_PANEL_H
#define EPD_PANEL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tps65185.h"
#include "epd_i80_bus.h"
#include "epd_panel_def.h"   /* EPD_PANEL_FLAG_*, epd_panel_def_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Waveform / update modes.
 * The panel driver translates these into its LUT / phase sequences.
 */
typedef enum {
    EPD_WAVEFORM_INIT = 0,  /**< Full init: clears ghosting, slow (~2 s)         */
    EPD_WAVEFORM_GC16,      /**< 16-level grayscale, best quality                */
    EPD_WAVEFORM_GL16,      /**< 16-level grayscale, reduced flash               */
    EPD_WAVEFORM_DU,        /**< Direct update: 2-level (B/W), fast (~0.3 s)     */
    EPD_WAVEFORM_A2,        /**< Animation: fastest 2-level, no flash            */
} epd_waveform_mode_t;

/** Opaque handle returned by epd_panel_create(). */
typedef struct epd_panel_dev *epd_panel_handle_t;

/**
 * Rectangular region in FRAMEBUFFER coordinates (before any mirror flags are
 * applied), used for partial updates.
 *
 * Because the source bus packs 4 pixels into every transferred byte, the
 * horizontal bounds are snapped OUTWARD to a multiple of 4 pixels before the
 * panel is driven.  The refreshed area is therefore always a superset of the
 * requested rectangle, never a subset.  Vertical bounds are exact.
 */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} epd_rect_t;

/**
 * Hardware description for one panel variant.
 * Fill this struct in your panel-specific source file (e.g. epd_display.c).
 */
typedef struct {
    uint16_t  width;        /**< Horizontal resolution in pixels               */
    uint16_t  height;       /**< Vertical resolution in pixels                 */
    uint8_t   bus_width;    /**< Source-bus width: 8 or 16                     */
    uint16_t  vcom_mv;      /**< VCOM voltage, positive mV (e.g. 1800 = -1.8V) */
    uint8_t   flags;        /**< EPD_PANEL_FLAG_* mirror bits                  */

    /**
     * Scale waveform frame counts by the panel temperature read from the PMIC
     * thermistor.  E-paper particle mobility falls sharply in the cold, so a
     * frame count tuned at room temperature under-drives below ~15 C and
     * over-drives above ~30 C.  Disable to keep frame counts fixed.
     */
    bool      temp_compensation;

    epd_i80_bus_config_t bus_cfg;   /**< i80 bus pin / clock configuration     */
    tps65185_handle_t    pmic;      /**< Initialised TPS65185 handle            */

    /**
     * Opaque pointer handed straight through to the panel driver's ops->init.
     *
     * The generic layer never dereferences this; it exists so a driver can
     * carry its own configuration (for epd_display.c, the epd_panel_def_t
     * describing geometry, AC timing and the waveform model) without this
     * header having to know about it.  Must stay valid for the panel's
     * lifetime.
     */
    const void *driver_cfg;
} epd_panel_config_t;

/**
 * Panel-specific operation table.
 *
 * Each panel driver (e.g. epd_display.c) fills this struct and passes it to
 * epd_panel_create().  The generic layer forwards calls through these pointers,
 * so swapping panels only requires a different ops table.
 */
typedef struct {
    /**
     * One-time hardware initialisation called inside epd_panel_create().
     * Optional — set to NULL if the panel needs no special init sequence.
     */
    esp_err_t (*init)(epd_panel_handle_t panel);

    /**
     * Release any resources allocated by init().
     * Called inside epd_panel_destroy().  Optional.
     */
    esp_err_t (*deinit)(epd_panel_handle_t panel);

    /**
     * Power rails on sequence:
     *   wake PMIC → wait power-good → set VCOM → enable VCOM buffer.
     */
    esp_err_t (*power_on)(epd_panel_handle_t panel);

    /**
     * Power rails off sequence:
     *   disable VCOM → power down PMIC → put PMIC in standby.
     */
    esp_err_t (*power_off)(epd_panel_handle_t panel);

    /**
     * Drive a display refresh, optionally restricted to a sub-rectangle.
     *
     * Pixel data is 4 bits-per-pixel, packed two pixels per byte
     * (high nibble = left pixel).  Buffer size = (width * height) / 2 bytes.
     *
     * @param prev_buf  Previous framebuffer state, i.e. what is currently on
     *                  the glass.  Only honoured by EPD_WAVEFORM_DU/A2: when
     *                  non-NULL the driver performs a genuine differential
     *                  update there, leaving unchanged pixels undriven and
     *                  driving changed pixels by the size and direction of the
     *                  change.  EPD_WAVEFORM_GC16/GL16 and EPD_WAVEFORM_INIT
     *                  ignore it and always assume the panel is at its white
     *                  baseline (as left by EPD_WAVEFORM_INIT), driving
     *                  absolute target levels.
     * @param next_buf  Target image to display.
     * @param mode      Waveform / quality selection.
     * @param area      Region to update, or NULL for the whole panel.
     */
    esp_err_t (*refresh)(epd_panel_handle_t panel,
                         const void *prev_buf,
                         const void *next_buf,
                         epd_waveform_mode_t mode,
                         const epd_rect_t *area);
} epd_panel_ops_t;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/**
 * @brief  Create a panel instance.
 *
 * Initialises the i80 bus and calls ops->init() if provided.
 *
 * @param config   Panel hardware description.
 * @param ops      Panel-specific operation table.
 * @param handle   Output handle.
 */
esp_err_t epd_panel_create(const epd_panel_config_t *config,
                            const epd_panel_ops_t    *ops,
                            epd_panel_handle_t       *handle);

/**
 * @brief  Destroy a panel instance and release all resources.
 */
esp_err_t epd_panel_destroy(epd_panel_handle_t handle);

/* ── Operations (thin wrappers that dispatch through ops table) ───────────── */

esp_err_t epd_panel_power_on(epd_panel_handle_t handle);
esp_err_t epd_panel_power_off(epd_panel_handle_t handle);

/**
 * @brief  Deep clean: repeated full black/white cycling to clear ghosting.
 *
 * A single EPD_WAVEFORM_INIT is already a black/white/black/white sequence and
 * is enough before an ordinary update.  This repeats that whole sequence, which
 * is what shifts ghosting an INIT leaves behind: particles that have sat in one
 * state for a long time, or a panel that has been updated many times without a
 * full clear, need several cycles before they fully release.
 *
 * The panel is left white, ready for an absolute GC16 update.
 *
 * Cost is roughly one INIT per cycle (about a second on a 1872x1404 panel), so
 * this is something to run on a schedule or on demand, not before every frame.
 * Three to five cycles clears most ghosting; ten is a thorough scrub.
 *
 * Requires the rails to be up - call epd_panel_power_on() first.
 *
 * @param cycles  Number of full cycles, clamped to at least 1.
 */
esp_err_t epd_panel_clean(epd_panel_handle_t handle, int cycles);

/**
 * @brief  Refresh the entire panel.
 *
 * Equivalent to epd_panel_refresh_area() with area = NULL.
 *
 * @param prev_buf  What is currently on the glass, or NULL to drive absolute
 *                  levels from the white baseline.  See epd_panel_ops_t.
 */
esp_err_t epd_panel_refresh(epd_panel_handle_t  handle,
                             const void         *prev_buf,
                             const void         *next_buf,
                             epd_waveform_mode_t mode);

/**
 * @brief  Refresh only part of the panel.
 *
 * Rows outside @p area are clocked out with no-drive data, so pixels there are
 * left undisturbed.  This is much faster than a full refresh and avoids
 * flashing the whole screen for a small change.
 *
 * Passing a non-NULL @p prev_buf together with @p area is the intended usage
 * for EPD_WAVEFORM_DU/A2: the driver then both restricts the region and drives
 * each changed pixel by its actual transition.  See epd_panel_ops_t::refresh.
 *
 * @param area  Region in framebuffer coordinates, or NULL for the full panel.
 *              Horizontal bounds are snapped outward to a 4-pixel boundary.
 *              A zero-area rectangle is a no-op that returns ESP_OK.
 */
esp_err_t epd_panel_refresh_area(epd_panel_handle_t  handle,
                                  const void         *prev_buf,
                                  const void         *next_buf,
                                  epd_waveform_mode_t mode,
                                  const epd_rect_t   *area);

/* ── Accessors for use inside panel drivers ──────────────────────────────── */

epd_i80_bus_handle_t      epd_panel_get_bus(epd_panel_handle_t handle);
tps65185_handle_t         epd_panel_get_pmic(epd_panel_handle_t handle);
const epd_panel_config_t *epd_panel_get_config(epd_panel_handle_t handle);

/**
 * @brief  Store panel-driver private data (called from ops->init).
 */
void epd_panel_set_priv(epd_panel_handle_t handle, void *priv);

/**
 * @brief  Retrieve panel-driver private data.
 */
void *epd_panel_get_priv(epd_panel_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* EPD_PANEL_H */
