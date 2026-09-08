/*
 * The wait between cycles, and what woke us.
 *
 * Selected at build time by CONFIG_HA_WAIT_DEEP_SLEEP / CONFIG_HA_WAIT_IDLE
 * (see Kconfig.projbuild) - deep sleep for battery life, idle for a bench
 * supply where the "refresh now" button matters more than runtime.
 *
 * WHAT DEEP SLEEP COSTS
 *
 * The button - and, in deep sleep specifically, the Home Assistant "refresh"
 * button too. SOC_RTCIO_PIN_COUNT is 16 on the P4, so only GPIO0-15 can drive
 * an ext0/ext1 wake; the button is GPIO35 and every low-numbered pin on rev.2
 * is spoken for (GPIO0/1 are the 32 kHz crystal, GPIO2-17 the EPD data bus).
 * There is no arrangement of this board that wakes it from a press, and with
 * the CPU off there is equally no way to hold an MQTT connection open to
 * catch a push from Home Assistant either - deep sleep only ever sees that
 * command on its next scheduled wake (see ha_mqtt.h's short command-subscribe
 * window). Idle builds don't have this problem: see ha_sleep_wait()'s
 * external_wake parameter.
 *
 * WHAT SURVIVES A SLEEP
 *
 * Deep sleep resets the CPU, so ordinary statics are reinitialised. The P4 has
 * RTC fast memory with no way to power it down, so RTC_DATA_ATTR is retained
 * across a sleep and reloaded from the image on a power cycle. Anything that
 * must survive a power cut as well goes in NVS instead (see ha_persist.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/epdinky_p4_board.h"

#include "ha_button.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief True when this build sleeps rather than idles. */
bool ha_sleep_is_deep(void);

/**
 * @brief  Which gesture ended the most recent ha_sleep_wait(), if any.
 *
 * HA_BUTTON_NONE after a plain timeout, and always in deep-sleep builds — the
 * button cannot be read while the CPU is off.
 */
ha_button_gesture_t ha_sleep_button_gesture(void);

/** @brief Short label for what woke this cycle, for logging: "timer",
 *  "button", "mqtt" or "powercycle". */
const char *ha_sleep_wake_reason(void);

/**
 * @brief  Wait @p seconds until the next cycle.
 *
 * In deep-sleep builds this does not return: the board powers down and the next
 * cycle starts in app_main(). In idle builds it returns after the wait, as
 * soon as the button is pressed and released, or as soon as @p external_wake
 * is seen set.
 *
 * @param  board  handles for the peripherals that have to be quietened first.
 *                Ignored when idling; NULL is tolerated.
 * @param  external_wake  polled alongside the button in idle builds (every
 *                100 ms); ending the wait immediately once true. Meant for a
 *                caller-owned MQTT listener (see main.c) so a Home Assistant
 *                "refresh now" press can end the wait the same way a
 *                physical short-press does, instead of only being caught in
 *                the brief window at the top of the next cycle. Ignored in
 *                deep-sleep builds - the CPU is off, there is nothing to
 *                poll - and NULL is tolerated in either build.
 */
void ha_sleep_wait(uint32_t seconds, const bsp_epdinky_handles_t *board,
                   volatile bool *external_wake);

#ifdef __cplusplus
}
#endif
