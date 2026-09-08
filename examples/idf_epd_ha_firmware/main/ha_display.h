/*
 * Panel ownership.
 *
 * Everything that touches the glass goes through here: the PMIC and panel
 * lifecycle, the single framebuffer, the fetched dashboard image, and the
 * locally rendered status screens the error paths need (Wi-Fi failed, MQTT
 * failed, image fetch failed, and provisioning). Nothing else in the
 * firmware holds an epd_panel_handle_t.
 *
 * Status screens are plain text, not a branded asset: this is a generic
 * example, not a specific product, so there is no logo to draw.
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
#include <stddef.h>
#include <stdint.h>

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
 *               module deliberately does not initialise the board itself:
 *               there is exactly one board init and main.c owns it.
 *
 * Powers the rails only for as long as each refresh needs them, so this is
 * cheap to call early. Safe to call twice; the second call is a no-op.
 */
esp_err_t ha_display_init(tps65185_handle_t pmic);

/** @brief The framebuffer, or NULL before ha_display_init() succeeds. */
epd_fb_t *ha_display_fb(void);

/**
 * @brief  Push the framebuffer to the glass.
 *
 * Powers up, runs INIT then GC16, powers down. @p clean adds a few full
 * black/white cycles first, which clears accumulated ghosting at the cost of
 * about a second and a visible flash - worth it on a cold boot or after a
 * status screen, not between routine dashboard updates.
 */
esp_err_t ha_display_flush(bool clean);

/**
 * @brief  Decode a fetched image into the framebuffer and show it.
 *
 * Wraps ha_image_render() plus ha_display_flush(). On a decode failure the
 * framebuffer is left untouched and nothing is drawn, so the panel keeps
 * showing the last good frame rather than going blank.
 */
esp_err_t ha_display_show_image(const uint8_t *data, size_t len, bool clean);

/**
 * @brief  Draw a status screen: a large heading, smaller body, footer.
 *
 * Any of heading/body/footer may be NULL. Text is centred horizontally and
 * the whole block is centred vertically. The built-in 8x8 font at an integer
 * scale is enough - these are diagnostics on a 1872x1404 (or 2200x1650)
 * panel, not typography.
 */
esp_err_t ha_display_message(const char *heading, const char *body, const char *footer);

/**
 * @brief  Draw the provisioning screen: heading, firmware version, and plain
 *         join instructions (SoftAP SSID and the portal URL).
 *
 * Deliberately plain text rather than a QR code: it keeps this module free
 * of an extra vendored dependency for what is, on a landscape panel this
 * size, an entirely readable SSID and URL.
 *
 * @param  ap_ssid     the SoftAP's SSID, e.g. "ha-epdinky-4d4cf0". Open
 *                     network.
 * @param  portal_url  where to browse once connected, e.g. "http://4.3.2.1/".
 * @param  fw_version  shown as "FW: <version>".
 */
esp_err_t ha_display_show_wifi_setup(const char *ap_ssid, const char *portal_url,
                                     const char *fw_version);

#ifdef __cplusplus
}
#endif
