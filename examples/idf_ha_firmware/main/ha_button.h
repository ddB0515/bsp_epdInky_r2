/*
 * Button gesture decoding — short / medium / long press on one GPIO.
 *
 * This board has exactly one physical button (SW4, GPIO35). Same three
 * gestures and thresholds as examples/idf_epd_ha_firmware/main/ha_button.h
 * (the decoding logic is unchanged), repurposed for an always-on touch
 * panel instead of a sleep/wake e-paper cycle:
 *
 *   - a short press (< 5 s): wake the backlight to full brightness
 *     immediately, without waiting for a touch - useful since dimming (see
 *     Kconfig HA_DISPLAY_DIM_PERCENT) means the panel isn't always at full
 *     brightness the way an e-paper display's "screen" always is;
 *   - a medium press (5-20 s): "reconfigure Home Assistant" - erases only
 *     the WebSocket host/port/token (Wi-Fi is left alone) and reboots,
 *     dropping the device back into the same "Wi-Fi connected - browse to
 *     http://<ip>/ to finish setup" config server a freshly Wi-Fi-provisioned
 *     device shows itself;
 *   - a long press (held past 20 s): "factory reset" - erases the Wi-Fi
 *     credentials too and reboots into the SoftAP setup portal. The one
 *     user-facing "start completely over" gesture.
 *
 * Unlike the e-paper firmware there is no "refresh now" concept here - the
 * dashboard is always live over its WebSocket connection, there is no cycle
 * to end early.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HA_BUTTON_NONE = 0,      /**< no gesture — the wait ended some other way. */
    HA_BUTTON_SHORT,         /**< wake the backlight to full brightness       */
    HA_BUTTON_RECONFIGURE,   /**< reconfigure Home Assistant (Wi-Fi kept)     */
    HA_BUTTON_FACTORY_RESET, /**< erase everything, including Wi-Fi          */
} ha_button_gesture_t;

/**
 * @brief  Resolve the gesture that has just started.
 *
 * Call once bsp_button_is_pressed() first reads true. Blocks until the
 * gesture is known and the button is released:
 *
 *   - held past 20 s -> returns HA_BUTTON_FACTORY_RESET as soon as the
 *     threshold is crossed, after waiting out the eventual release so the
 *     same press cannot also register as something else;
 *   - released between 5 s and 20 s -> HA_BUTTON_RECONFIGURE;
 *   - released before 5 s -> HA_BUTTON_SHORT.
 */
ha_button_gesture_t ha_button_wait_gesture(void);

#ifdef __cplusplus
}
#endif
