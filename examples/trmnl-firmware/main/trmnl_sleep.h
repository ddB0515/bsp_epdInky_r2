/*
 * The wait between cycles, and what woke us.
 *
 * Upstream deep-sleeps on a timer and treats every wake as a fresh boot. This
 * port can do that, but it can also idle - selected at build time by
 * CONFIG_TRMNL_WAIT_DEEP_SLEEP / CONFIG_TRMNL_WAIT_IDLE - because the two modes
 * answer different questions and only one of them keeps the button working.
 *
 * WHY BOTH MODES EXIST
 *
 * This board's deep-sleep current is unmeasured, and there is reason to think
 * it will not match upstream's: the ESP32-C6's EN line is pulled up, so the
 * radio does not necessarily lose power when the P4 sleeps. Whether the
 * server's 15-minute duty cycle is achievable on a battery depends on that
 * number, and the only way to get it is to meter the board in each mode.
 *
 * WHAT DEEP SLEEP COSTS
 *
 * The button. SOC_RTCIO_PIN_COUNT is 16 on the P4, so only GPIO0-15 can drive
 * an ext0/ext1 wake; the button is GPIO35 and every low-numbered pin on rev.2
 * is spoken for (GPIO0/1 are the 32 kHz crystal, GPIO2-17 the EPD data bus).
 * There is no arrangement of this board that wakes it from a press. In deep
 * sleep the only ways to get an early frame are RESET and waiting.
 *
 * WHAT SURVIVES A SLEEP
 *
 * Deep sleep resets the CPU, so ordinary statics are reinitialised. The P4 has
 * RTC fast memory and no way to power it down (soc_caps.h defines
 * SOC_RTC_FAST_MEM_SUPPORTED but not SOC_PM_SUPPORT_RTC_FAST_MEM_PD), so
 * RTC_DATA_ATTR is retained across a sleep and reloaded from the image on a
 * power cycle - exactly the lifetime the retry counters want. Anything that
 * must survive a power cut as well goes in NVS instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp/epdinky_p4_board.h"

#include "trmnl_button.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief True when this build sleeps rather than idles. */
bool trmnl_sleep_is_deep(void);

/**
 * @brief  Which gesture ended the most recent trmnl_sleep_wait(), if any.
 *
 * TRMNL_BUTTON_NONE after a plain timeout, and always in deep-sleep builds —
 * the button cannot be read while the CPU is off, so there is nothing to
 * resolve a gesture from. See trmnl_button.h for what each value means.
 */
trmnl_button_gesture_t trmnl_sleep_button_gesture(void);

/**
 * @brief  The Update-Source header value for this cycle.
 *
 * "powercycle" on a cold boot, "timer" after a timed wake or a completed idle
 * wait, "button" when the idle wait was cut short by a press. The spellings are
 * upstream's wakeupReasonMap (lib/trmnl/src/logging_parsers.cpp) - the server
 * has seen these exact strings from every TRMNL device, so they are not the
 * place to be inventive.
 */
const char *trmnl_sleep_update_source(void);

/**
 * @brief  Wait @p seconds until the next cycle.
 *
 * In deep-sleep builds this does not return: the board powers down and the next
 * cycle starts in app_main(). In idle builds it returns after the wait, or as
 * soon as the button is pressed and released.
 *
 * @param  board  handles for the peripherals that have to be quietened first.
 *                Ignored when idling; NULL is tolerated.
 */
void trmnl_sleep_wait(uint32_t seconds, const bsp_epdinky_handles_t *board);

#ifdef __cplusplus
}
#endif
