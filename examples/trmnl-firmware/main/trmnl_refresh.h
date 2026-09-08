/*
 * How long to wait before the next cycle — upstream's RefreshInterval.
 *
 * The server drives the cadence in the normal case (`refresh_rate` from
 * /api/display), and the ladders below take over when something goes wrong so a
 * failing device backs off instead of hammering the API. The interval is kept
 * in NVS rather than in RAM because upstream deep-sleeps between cycles and has
 * nowhere else to put it; this port idles for now (see main.c), but keeping the
 * value in NVS means switching to deep sleep changes nothing here.
 *
 * The numbers are upstream's, not invented:
 *
 *   API retry      attempt 1 -> 15 s, 2 -> 30 s, 3 -> 60 s, beyond -> default
 *   Wi-Fi retry    attempt 1 -> 60 s, 2 -> 180 s, 3 and beyond -> 300 s
 *   fast poll      streak <= 50 -> 5 s, <= 60 -> 60 s, <= 70 -> 900 s, else 1 h
 *   default        900 s
 *
 * "Fast poll" is the state where the server has accepted us but has nothing to
 * show — HTTP 202, HTTP 500, or a device with no plugin attached. Polling every
 * 5 s makes a freshly set-up device feel responsive while the user is attaching
 * a plugin in the web UI; the streak counter is what stops that lasting forever
 * if they walk away.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The interval currently in force, in seconds. */
uint32_t trmnl_refresh_seconds(void);

/** @brief Adopt the server's `refresh_rate`. Stored verbatim, as upstream does. */
uint32_t trmnl_refresh_apply_server_rate(uint32_t rate);

/** @brief Back off after a failed API call. @p attempt counts from 1. */
uint32_t trmnl_refresh_apply_api_retry(uint8_t attempt);

/** @brief Back off after a failed Wi-Fi association. @p attempt counts from 1. */
uint32_t trmnl_refresh_apply_wifi_retry(uint8_t attempt);

/**
 * @brief  Poll quickly while the server has nothing to show, decaying over time.
 *
 * Advances the streak counter, so repeated calls lengthen the interval.
 */
uint32_t trmnl_refresh_apply_fast_poll(void);

/** @brief Fall back to the fixed default. */
uint32_t trmnl_refresh_apply_default(void);

/** @brief Clear the fast-poll streak; call whenever a real image arrives. */
void trmnl_refresh_reset_fast_poll_streak(void);

#ifdef __cplusplus
}
#endif
