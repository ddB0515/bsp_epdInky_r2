/*
 * Button gesture decoding — short / double / long press on one GPIO.
 *
 * This board has exactly one button (SW4, GPIO35), so every gesture upstream's
 * dashboard exposes has to be told apart by timing alone rather than by which
 * button was pressed. That is a firmware problem, not a hardware limit:
 *
 *   - a short press, released well before the long-press threshold;
 *   - a double press, a second short press starting within the double-click
 *     window of the first release;
 *   - a long press, held past the threshold — upstream's "reset button held
 *     > 5000 ms" gesture.
 *
 * This module only decodes; it does not act. Nothing here touches Wi-Fi, NVS
 * or the display — see trmnl_sleep.c for where a gesture ends the idle wait,
 * and main.c's run_cycle() for what a double- or long-press does once one has.
 *
 * Only reachable from the idle build. In deep sleep the CPU is off between
 * cycles, so there is nothing to poll — see trmnl_sleep.h.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRMNL_BUTTON_NONE = 0,  /**< no gesture — the wait ended some other way. */
    TRMNL_BUTTON_SINGLE,
    TRMNL_BUTTON_DOUBLE,
    TRMNL_BUTTON_LONG,
} trmnl_button_gesture_t;

/**
 * @brief  Resolve the gesture that has just started.
 *
 * Call the instant bsp_button_is_pressed() first reads true. Blocks until the
 * gesture is known and the button is released:
 *
 *   - held past the long-press threshold -> returns TRMNL_BUTTON_LONG as soon
 *     as the threshold is crossed, after waiting out the eventual release so
 *     the same press cannot also register as something else;
 *   - released before the threshold -> waits out the double-click window for
 *     a second press. One arrives -> TRMNL_BUTTON_DOUBLE once it too is
 *     released. None arrives -> TRMNL_BUTTON_SINGLE once the window closes.
 *
 * Worst case this blocks for the long-press threshold; every other path is
 * bounded by the double-click window on top of however long the user's own
 * press lasted.
 */
trmnl_button_gesture_t trmnl_button_wait_gesture(void);

#ifdef __cplusplus
}
#endif
