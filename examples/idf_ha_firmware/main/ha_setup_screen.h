/*
 * The two provisioning-stage screens, drawn with LVGL directly onto the
 * panel - the equivalent of examples/idf_epd_ha_firmware/main/ha_display.c's
 * ha_display_show_wifi_setup()/ha_display_message() for this panel, using
 * LVGL's own lv_qrcode widget instead of vendoring a QR encoder (see
 * sdkconfig.defaults's CONFIG_LV_USE_QRCODE comment for why).
 *
 * Separate from ha_dashboard.c: the dashboard doesn't exist yet at the point
 * either of these screens is shown, and ha_portal.c (server logic) has no
 * business owning widget code directly.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stage-1 screen: SoftAP SSID, a scannable WIFI: QR code, portal URL. */
void ha_setup_screen_show_wifi(const char *ap_ssid, const char *portal_url);

/** @brief Stage-2 screen: "Wi-Fi connected - open this address to finish setup". */
void ha_setup_screen_show_config(const char *portal_url);

/** @brief A plain status screen - used for the Wi-Fi retry-in-place message. */
void ha_setup_screen_show_message(const char *title, const char *body);

#ifdef __cplusplus
}
#endif
