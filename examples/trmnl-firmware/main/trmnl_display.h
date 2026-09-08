/*
 * Panel ownership for the TRMNL port.
 *
 * Everything that touches the glass goes through here: the PMIC and panel
 * lifecycle, the single framebuffer, the server image, and the locally rendered
 * status screens that the state machine's error paths need. Nothing else in the
 * firmware holds an epd_panel_handle_t.
 *
 * Two rules the epd component imposes, both enforced inside:
 *
 *   - EPD_WAVEFORM_INIT must precede EPD_WAVEFORM_GC16. GC16's phases can only
 *     darken, so they assume a white baseline; without the init pass a frame
 *     comes out muddied by whatever was there before.
 *   - the rails must come down after a refresh. With them up VCOM sits live and
 *     white areas visibly go grainy within minutes - it is not just a power
 *     question.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "epd_fb.h"
#include "tps65185.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Bring up the panel and allocate the framebuffer.
 *
 * @param  pmic  the TPS65185 handle from bsp_epdinky_init_with_config(). This
 *               module deliberately does not initialise the board itself: the
 *               cache needs the expander and the clock needs the RTC, so there
 *               is exactly one board init and main.c owns it.
 *
 * Powers the rails only for as long as each refresh needs them, so this is
 * cheap to call early. Safe to call twice; the second call is a no-op.
 */
esp_err_t trmnl_display_init(tps65185_handle_t pmic);

/** @brief The framebuffer, or NULL before trmnl_display_init() succeeds. */
epd_fb_t *trmnl_display_fb(void);

/**
 * @brief  Push the framebuffer to the glass.
 *
 * Powers up, runs INIT then GC16, powers down. @p clean adds a few full
 * black/white cycles first, which clears accumulated ghosting at the cost of
 * about a second and a visible flash - worth it on a cold boot, not between
 * routine updates.
 */
esp_err_t trmnl_display_flush(bool clean);

/**
 * @brief  Decode an image from the server into the framebuffer and show it.
 *
 * Wraps trmnl_image_render() plus trmnl_display_flush(). On a decode failure
 * the framebuffer is left untouched and nothing is drawn, so the panel keeps
 * showing the last good frame rather than going blank.
 */
esp_err_t trmnl_display_show_image(const uint8_t *data, size_t len, bool clean);

/**
 * @brief  Draw a status screen: the TRMNL mark, a large heading, smaller
 *         body, footer.
 *
 * Text replaces most of upstream's G5-compressed bitmap assets, by decision
 * 5 - the mark itself (trmnl_logo.h) is the one exception, so these screens
 * still read as this device rather than a generic diagnostic dump. Any of
 * heading/body/footer may be NULL. Text is centred horizontally and the
 * whole block, mark included, is centred vertically.
 */
esp_err_t trmnl_display_message(const char *heading, const char *body, const char *footer);

/**
 * @brief  Word-wrap @p text into the bottom margin of whatever is already in
 *         the framebuffer, then flush.
 *
 * For overlaying a caption on a system-served image (the "Register at
 * trmnl.com/start with Device ID ..." message that accompanies /api/setup's
 * setup-screen image_url) rather than replacing it - the one status screen
 * that is a server image plus local text instead of one or the other.
 *
 * No clean: whatever put the image up already did one, and adding a caption
 * under it is not the kind of change that ghosts.
 */
esp_err_t trmnl_display_caption(const char *text);

/**
 * @brief  Draw the provisioning screen: heading, firmware version, join
 *         instructions and a QR code that offers to join the SoftAP directly.
 *
 * @param  ap_ssid      the SoftAP's SSID, e.g. "TRMNL-4D4CF0". Open network -
 *                      the QR payload is WIFI:T:nopass;S:<ssid>;;.
 * @param  fw_version   shown as "FW: <version>".
 */
esp_err_t trmnl_display_show_wifi_setup(const char *ap_ssid, const char *fw_version);

#ifdef __cplusplus
}
#endif
