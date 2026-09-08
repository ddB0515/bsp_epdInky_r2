/*
 * LVGL on a raw E Ink panel.
 *
 * LVGL and e-paper want opposite things. LVGL assumes it can flush a small
 * dirty rectangle cheaply and often; this panel needs a full INIT + GC16 pass
 * costing about 1.4 s, and its DU partial update is unusable (a source line
 * that drives VPOS retains the charge and bleaches the rest of its column).
 *
 * So the two are decoupled:
 *
 *   LVGL  ->  L8 shadow buffer  ->  [refresh policy]  ->  4bpp  ->  panel
 *
 * LVGL renders at its own pace into an 8-bit greyscale buffer in PSRAM and
 * never waits for the glass. The flush callback only records that something
 * changed. A separate policy decides when to actually pay for a refresh, and
 * that is where the panel's cost is confined.
 *
 * Rendering in L8 rather than RGB565 is what makes the conversion trivial: the
 * panel wants 4 bits of grey per pixel, so a byte of L8 becomes a nibble with
 * one shift. No colour space conversion and no dithering pass.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "epd_panel.h"

#ifdef __cplusplus
extern "C" {
#endif

/** How a pending change should reach the glass. */
typedef enum {
    /**
     * Full clear then absolute greyscale: INIT + GC16, about 1.4 s.
     *
     * The only mode this panel renders correctly, and the default. GC16 phases
     * can only darken, so the INIT is what supplies the white baseline they
     * assume - dropping it leaves the previous image showing through.
     */
    APP_EPD_LVGL_FULL = 0,

    /**
     * Greyscale without the preceding clear, about 0.4 s.
     *
     * Correct only when the panel is already white where the new image is
     * lighter than the old one, so it suits adding content to a blank screen.
     * Anything else ghosts.
     */
    APP_EPD_LVGL_GC16_ONLY,
} app_epd_lvgl_mode_t;

typedef struct {
    /** Panel to draw on. Must already be created and powered on. */
    epd_panel_handle_t panel;

    /**
     * Quiet period before a refresh, in milliseconds.
     *
     * A single LVGL interaction produces several flushes - a button redraws
     * pressed, then released, then its label - and refreshing on each would
     * cost seconds and flash the screen repeatedly. Waiting for the screen to
     * stop changing coalesces them into one refresh.
     *
     * Too short and a multi-step animation refreshes several times; too long
     * and the panel feels unresponsive. 400 ms is a reasonable start.
     */
    uint32_t settle_ms;

    /**
     * Longest a visible change may wait, in milliseconds, or 0 for no limit.
     *
     * Guards against a UI that never goes quiet - a running clock or spinner
     * would otherwise reset the settle timer forever and never reach the
     * glass.
     */
    uint32_t max_defer_ms;

    /** Waveform used for automatic refreshes. */
    app_epd_lvgl_mode_t mode;

    /**
     * Leave the panel rails powered between updates.
     *
     * Off by default, and that default matters: with the rails up, VCOM sits
     * live at its operating voltage while the source lines rest near ground,
     * so every pixel sees a continuous DC field of the full VCOM magnitude.
     * Over minutes the pigment drifts under it and uniform white areas go
     * visibly grainy. Powering down between updates parks the panel neutral.
     *
     * Setting this saves the power-up sequence - roughly 230 ms against a
     * 1.4 s refresh - and is only reasonable for a UI updating more or less
     * continuously, where the panel never sits idle under bias for long.
     *
     * Note the symptom scales with VCOM, so it hides completely on a panel
     * whose VCOM is mis-set near zero and appears once VCOM is correct.
     */
    bool keep_rails_on;

    /**
     * Deep-clean cycles to run before the first screen, or 0 to skip.
     *
     * Done by the LVGL task rather than the caller because the panel has a
     * single owner: the EPD bus serialises transfers with a mutex taken with
     * portMAX_DELAY, so a refresh issued from a second task at the same time
     * would block one of them with no diagnostic.
     */
    int startup_clean_cycles;
} app_epd_lvgl_config_t;

/**
 * @brief Attach LVGL to an EPD panel.
 *
 * Calls lv_init(), creates the display and allocates the L8 shadow buffer from
 * PSRAM. Does not start a task: call app_epd_lvgl_run() from one, or drive
 * app_epd_lvgl_timer_handler() yourself.
 */
esp_err_t app_epd_lvgl_init(const app_epd_lvgl_config_t *cfg);

/**
 * @brief Start the LVGL task.
 *
 * Runs LVGL's timers and the refresh policy on a dedicated task with a stack
 * large enough for LVGL's rendering call depth - the default main task stack
 * is not, and overflows with a stack protection fault on the first refresh.
 *
 * Returns as soon as the task exists; the first screen appears a moment later.
 */
esp_err_t app_epd_lvgl_start(void);

/**
 * @brief Run LVGL's timers and the refresh policy forever, on the calling task.
 *
 * Does the startup clean, shows the first screen, then refreshes whenever the
 * settle or defer rules are satisfied. app_epd_lvgl_start() calls this on a
 * task of its own; call it directly only if you want to own the task yourself.
 */
void app_epd_lvgl_run(void);

/**
 * @brief Push whatever LVGL has drawn to the glass now.
 *
 * Bypasses the settle timer. Blocks for the length of a refresh - over a
 * second for a full one - so it is for deliberate moments such as the first
 * screen. Do not call it from an LVGL event callback, which runs with the
 * lock held inside lv_timer_handler(); use app_epd_lvgl_request_refresh().
 */
esp_err_t app_epd_lvgl_refresh_now(app_epd_lvgl_mode_t mode);

/** @brief Deep clean, then redraw whatever LVGL currently has. */
esp_err_t app_epd_lvgl_clean(int cycles);

/**
 * @brief Ask the policy loop for a full refresh at the next opportunity.
 *
 * Safe from an LVGL event callback: it only raises a flag, so the callback
 * returns immediately and the refresh happens once lv_timer_handler() is done.
 */
void app_epd_lvgl_request_refresh(void);

/** @brief Ask the policy loop for a deep clean followed by a redraw. */
void app_epd_lvgl_request_clean(void);

/**
 * @brief Take the LVGL lock.
 *
 * LVGL is not thread safe, so anything touching it from outside the LVGL task
 * must hold this. Pass 0 to wait indefinitely.
 */
bool app_epd_lvgl_lock(uint32_t timeout_ms);

/** @brief Release the LVGL lock. */
void app_epd_lvgl_unlock(void);

/** @brief The LVGL display, for lv_display_* calls. */
lv_display_t *app_epd_lvgl_display(void);

#ifdef __cplusplus
}
#endif
