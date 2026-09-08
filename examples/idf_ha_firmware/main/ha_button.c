#include "ha_button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/epdinky_p4_board.h"

/* The two held-button thresholds. */
#define MEDIUM_PRESS_MS 5000
#define LONG_PRESS_MS   20000

/* How often the level is sampled while resolving a gesture. */
#define POLL_MS 20

static void wait_while_pressed(void)
{
    while (bsp_button_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

ha_button_gesture_t ha_button_wait_gesture(void)
{
    uint32_t held_ms = 0;
    while (bsp_button_is_pressed()) {
        if (held_ms >= LONG_PRESS_MS) {
            wait_while_pressed();
            return HA_BUTTON_FACTORY_RESET;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        held_ms += POLL_MS;
    }

    if (held_ms >= MEDIUM_PRESS_MS) {
        return HA_BUTTON_RECONFIGURE;
    }
    return HA_BUTTON_SHORT;
}
