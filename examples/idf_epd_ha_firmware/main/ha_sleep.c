#include "ha_sleep.h"

#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"

#include "tps65185.h"

#include "ha_button.h"

static const char *TAG = "ha_sleep";

/* How often the idle wait looks at the button. Fast enough that a deliberate
 * press is never missed, slow enough to be invisible in the power figures. */
#define BUTTON_POLL_MS 100

/* Latched on first use; see ha_sleep_wake_reason(). */
static const char *s_reason;

/* Set by wait_idle() when a gesture ends the wait; HA_BUTTON_NONE otherwise,
 * including for the whole lifetime of a deep-sleep build. */
static ha_button_gesture_t s_gesture = HA_BUTTON_NONE;

/* -------------------------------------------------------------------------- */

bool ha_sleep_is_deep(void)
{
#if CONFIG_HA_WAIT_DEEP_SLEEP
    return true;
#else
    return false;
#endif
}

const char *ha_sleep_wake_reason(void)
{
    if (s_reason == NULL) {
        /*
         * A bitmap, not an enum: several sources can fire together, and
         * esp_sleep_get_wakeup_cause() - which flattens them to one - is
         * deprecated for exactly that reason. On a reset that was not a wake
         * the only bit set is ESP_SLEEP_WAKEUP_UNDEFINED.
         */
        const uint32_t causes = esp_sleep_get_wakeup_causes();

        if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            s_reason = "timer";
        } else if (causes & (BIT(ESP_SLEEP_WAKEUP_GPIO) | BIT(ESP_SLEEP_WAKEUP_EXT1))) {
            /* Unreachable on this board - the button is not an RTC pin - but
             * cheap to keep correct in case a future revision moves it. */
            s_reason = "button";
        } else {
            s_reason = "powercycle";
        }
    }
    return s_reason;
}

ha_button_gesture_t ha_sleep_button_gesture(void)
{
    return s_gesture;
}

/* -------------------------------------------------------------------------- */
/* Idle                                                                       */
/* -------------------------------------------------------------------------- */

#if !CONFIG_HA_WAIT_DEEP_SLEEP

static void wait_idle(uint32_t seconds, volatile bool *external_wake)
{
    ESP_LOGI(TAG, "next cycle in %" PRIu32 " s (idling; press the button, or press "
             "Refresh Now in Home Assistant, for one now)", seconds);

    /* Cleared on every wait, so a plain timeout - or a gesture from a wait
     * two cycles ago - never leaks into this one. */
    s_gesture = HA_BUTTON_NONE;

    const uint32_t ticks_total = pdMS_TO_TICKS(seconds * 1000u);
    const uint32_t ticks_step  = pdMS_TO_TICKS(BUTTON_POLL_MS);

    for (uint32_t waited = 0; waited < ticks_total; waited += ticks_step) {
        if (bsp_button_is_pressed()) {
            /* ha_button_wait_gesture() blocks until it knows which gesture
             * this is and the button has been released, so there is nothing
             * left here to wait out afterwards. */
            s_gesture = ha_button_wait_gesture();
            s_reason  = "button";

            const char *what = "pressed; refreshing now";
            if (s_gesture == HA_BUTTON_FACTORY_RESET) {
                what = "held >20 s (factory reset)";
            } else if (s_gesture == HA_BUTTON_RECONFIGURE) {
                what = "held 5-20 s (reconfigure MQTT/Home Assistant)";
            }
            ESP_LOGI(TAG, "button %s", what);
            return;
        }
        if (external_wake != NULL && *external_wake) {
            s_reason = "mqtt";
            ESP_LOGI(TAG, "Home Assistant requested a refresh; waking early");
            return;
        }
        vTaskDelay(ticks_step);
    }

    s_reason = "timer";
}

#else /* CONFIG_HA_WAIT_DEEP_SLEEP */

/* -------------------------------------------------------------------------- */
/* Deep sleep                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * Everything that draws current and is not switched off by the sleep itself.
 *
 * Deep sleep cuts the digital core, but not the rest of the board: the PMIC
 * keeps its own regulators alive, and the C6 has a pulled-up EN line and no
 * idea the P4 has gone. Each of these has to be told.
 */
static void power_down(const bsp_epdinky_handles_t *board)
{
    /*
     * bsp_wifi_deinit() stops the station and takes the esp-hosted link
     * down, which is the only signal the C6 gets that it can stop
     * listening. The next boot brings it back up from scratch.
     */
    bsp_wifi_disconnect();
    bsp_wifi_deinit();

    /*
     * The panel rails are already down: ha_display_flush() powers them off
     * after every refresh, and epd_display_power_off() leaves the TPS65185
     * in standby. Standby still keeps the device awake and answering on
     * I2C, so follow it to the real sleep state, which de-asserts WAKEUP.
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

#endif /* CONFIG_HA_WAIT_DEEP_SLEEP */

/* -------------------------------------------------------------------------- */

void ha_sleep_wait(uint32_t seconds, const bsp_epdinky_handles_t *board,
                   volatile bool *external_wake)
{
    /*
     * A zero interval would busy-loop in idle builds and, in deep-sleep
     * builds, turn into a reboot storm that is awkward to interrupt.
     */
    if (seconds == 0) {
        seconds = 1;
    }

#if CONFIG_HA_WAIT_DEEP_SLEEP
    (void)external_wake; /* the CPU is off; nothing can poll it */
    wait_deep_sleep(seconds, board);
#else
    (void)board;
    wait_idle(seconds, external_wake);
#endif
}
