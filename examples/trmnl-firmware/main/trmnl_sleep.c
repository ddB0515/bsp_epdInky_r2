#include "trmnl_sleep.h"

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"

#include "tps65185.h"

#include "trmnl_button.h"
#include "trmnl_cache.h"

static const char *TAG = "trmnl_sleep";

/* How often the idle wait looks at the button. Fast enough that a deliberate
 * press is never missed, slow enough to be invisible in the power figures. */
#define BUTTON_POLL_MS 100

/* Latched on first use; see trmnl_sleep_update_source(). */
static const char *s_source;

/* Set by wait_idle() when a gesture ends the wait; TRMNL_BUTTON_NONE
 * otherwise, including for the whole lifetime of a deep-sleep build. */
static trmnl_button_gesture_t s_gesture = TRMNL_BUTTON_NONE;

/* -------------------------------------------------------------------------- */

bool trmnl_sleep_is_deep(void)
{
#if CONFIG_TRMNL_WAIT_DEEP_SLEEP
    return true;
#else
    return false;
#endif
}

const char *trmnl_sleep_update_source(void)
{
    if (s_source == NULL) {
        /*
         * A bitmap, not an enum: several sources can fire together, and
         * esp_sleep_get_wakeup_cause() - which flattens them to one - is
         * deprecated for exactly that reason. On a reset that was not a wake
         * the only bit set is ESP_SLEEP_WAKEUP_UNDEFINED.
         */
        const uint32_t causes = esp_sleep_get_wakeup_causes();

        if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            s_source = "timer";
        } else if (causes & (BIT(ESP_SLEEP_WAKEUP_GPIO) | BIT(ESP_SLEEP_WAKEUP_EXT1))) {
            /* Unreachable on this board - the button is not an RTC pin - but
             * cheap to keep correct in case a future revision moves it. */
            s_source = "button";
        } else {
            s_source = "powercycle";
        }
    }
    return s_source;
}

trmnl_button_gesture_t trmnl_sleep_button_gesture(void)
{
    return s_gesture;
}

/* -------------------------------------------------------------------------- */
/* Idle                                                                       */
/* -------------------------------------------------------------------------- */

#if !CONFIG_TRMNL_WAIT_DEEP_SLEEP

static void wait_idle(uint32_t seconds)
{
    ESP_LOGI(TAG, "next cycle in %" PRIu32 " s (idling; press the button for one now)",
             seconds);

    /* Cleared on every wait, so a plain timeout - or a gesture from a wait
     * two cycles ago - never leaks into this one. */
    s_gesture = TRMNL_BUTTON_NONE;

    const uint32_t ticks_total = pdMS_TO_TICKS(seconds * 1000u);
    const uint32_t ticks_step  = pdMS_TO_TICKS(BUTTON_POLL_MS);

    for (uint32_t waited = 0; waited < ticks_total; waited += ticks_step) {
        if (bsp_button_is_pressed()) {
            /* trmnl_button_wait_gesture() blocks until it knows which gesture
             * this is and the button has been released, so there is nothing
             * left here to wait out afterwards. */
            s_gesture = trmnl_button_wait_gesture();
            s_source  = "button";

            switch (s_gesture) {
            case TRMNL_BUTTON_LONG:
                ESP_LOGI(TAG, "button held >5 s (ClearWifi gesture)");
                break;
            case TRMNL_BUTTON_DOUBLE:
                ESP_LOGI(TAG, "button double-clicked; refreshing now");
                break;
            default:
                ESP_LOGI(TAG, "button pressed; refreshing now");
                break;
            }
            return;
        }
        vTaskDelay(ticks_step);
    }

    s_source = "timer";
}

#else /* CONFIG_TRMNL_WAIT_DEEP_SLEEP */

/* -------------------------------------------------------------------------- */
/* Deep sleep                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * Everything that draws current and is not switched off by the sleep itself.
 *
 * Deep sleep cuts the digital core, but not the rest of the board: the card
 * keeps its supply through the expander, the PMIC keeps its own regulators
 * alive, and the C6 has a pulled-up EN line and no idea the P4 has gone. Each
 * of these has to be told.
 */
static void power_down(const bsp_epdinky_handles_t *board)
{
    /* The card first, because it is the only one that can lose data. */
    trmnl_cache_deinit();

    /*
     * Then the radio. bsp_wifi_deinit() stops the station and takes the
     * esp-hosted link down, which is the only signal the C6 gets that it can
     * stop listening. The next boot brings it back up from scratch - about
     * 1.5 s, already part of every cycle's cost.
     */
    bsp_wifi_disconnect();
    bsp_wifi_deinit();

    /*
     * The panel rails are already down: trmnl_display_flush() powers them off
     * after every refresh, and epd_display_power_off() leaves the TPS65185 in
     * standby. Standby still keeps the device awake and answering on I2C, so
     * follow it to the real sleep state, which de-asserts WAKEUP.
     *
     * GPIO37 (WAKEUP) floats once the P4 sleeps, so the PMIC would end up here
     * regardless - doing it deliberately just means it happens while the pin is
     * still driven rather than during the transition. epd_display_power_on()
     * calls tps65185_wakeup() on the way back, so nothing else has to know.
     */
    if (board != NULL && board->tps65185 != NULL) {
        tps65185_sleep(board->tps65185);
    }
}

static void wait_deep_sleep(uint32_t seconds, const bsp_epdinky_handles_t *board)
{
    ESP_LOGI(TAG, "sleeping %" PRIu32 " s (the button will not wake it; press RESET)",
             seconds);

    power_down(board);

    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
    /* Not reached. */
}

#endif /* CONFIG_TRMNL_WAIT_DEEP_SLEEP */

/* -------------------------------------------------------------------------- */

void trmnl_sleep_wait(uint32_t seconds, const bsp_epdinky_handles_t *board)
{
    /*
     * A zero interval would busy-loop in idle builds and, in deep-sleep builds,
     * turn into a reboot storm that is awkward to interrupt. The callers already
     * avoid storing a zero rate; this is the backstop.
     */
    if (seconds == 0) {
        seconds = 1;
    }

#if CONFIG_TRMNL_WAIT_DEEP_SLEEP
    wait_deep_sleep(seconds, board);
#else
    (void)board;
    wait_idle(seconds);
#endif
}
