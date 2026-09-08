#include "trmnl_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/epdinky_p4_board.h"

/* Upstream's own threshold for the held-button ("ClearWifi") gesture. */
#define LONG_PRESS_MS 5000

/* How long to wait, after a release, for a second press to start. Long enough
 * that a deliberate double-click never misses it; short enough that a single
 * press does not leave the user waiting to see the panel react. */
#define DOUBLE_CLICK_WINDOW_MS 400

/* How often the level is sampled while resolving a gesture. Coarser than
 * trmnl_sleep.c's idle-wait poll (which has a whole refresh interval to fill)
 * because a gesture has to be resolved within a few hundred milliseconds. */
#define POLL_MS 20

static void wait_while_pressed(void)
{
    while (bsp_button_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

trmnl_button_gesture_t trmnl_button_wait_gesture(void)
{
    uint32_t held_ms = 0;
    while (bsp_button_is_pressed()) {
        if (held_ms >= LONG_PRESS_MS) {
            wait_while_pressed();
            return TRMNL_BUTTON_LONG;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        held_ms += POLL_MS;
    }

    /* Released before the long-press threshold: a short press. Give a second
     * one a chance to start before settling on "single". */
    uint32_t waited_ms = 0;
    while (waited_ms < DOUBLE_CLICK_WINDOW_MS) {
        if (bsp_button_is_pressed()) {
            wait_while_pressed();
            return TRMNL_BUTTON_DOUBLE;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        waited_ms += POLL_MS;
    }

    return TRMNL_BUTTON_SINGLE;
}
