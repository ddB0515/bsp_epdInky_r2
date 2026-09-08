/*
 * Wall-clock time.
 *
 * Two sources, in the order that matters for a device that spends most of its
 * life asleep:
 *
 *   - the RV-3028 on the I2C bus is the one that survives. It is battery-backed
 *     and keeps counting whether or not the P4 is running, so it is read first,
 *     at boot, before the network exists;
 *   - SNTP corrects it. The RV-3028 drifts a few seconds a month, and the P4's
 *     own system clock is worthless across a reset, so every successful network
 *     cycle is an opportunity to write the truth back to the RTC.
 *
 * The result is that time is available immediately at boot (from the RTC) and
 * stays accurate over months (from SNTP), rather than being unknown until the
 * first successful network round trip - which matters here for the MQTT
 * "last refresh" timestamp sensor.
 *
 * Everything is UTC. No timezone is set - Home Assistant renders whatever
 * timestamp it is given in the user's own configured timezone.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#include "rv3028.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Seed the system clock from the RTC. Call before the network is up.
 *
 * @param  rtc  the RV-3028 handle, or NULL if the part did not respond — the
 *              board treats it as optional, so that is not fatal and the clock
 *              simply stays unset until SNTP runs.
 */
esp_err_t ha_time_init(rv3028_handle_t rtc);

/**
 * @brief  Sync from SNTP and write the result back to the RTC.
 *
 * Blocks up to @p timeout_ms. Failure is not fatal — the clock keeps whatever
 * the RTC gave it. Cheap enough to call once per cycle, but see
 * ha_time_sync_due().
 */
esp_err_t ha_time_sync_sntp(uint32_t timeout_ms);

/**
 * @brief  Whether a sync is worth doing this cycle.
 *
 * True when the clock has never been set, or when the last successful sync was
 * long enough ago that the RTC's drift is worth correcting.
 */
bool ha_time_sync_due(void);

/** @brief True once the clock holds a plausible date rather than the epoch. */
bool ha_time_is_valid(void);

/** @brief UTC as "YYYY-MM-DD HH:MM:SS", or "(no clock)" if unset. */
void ha_time_str(char *out, size_t len);

/**
 * @brief  @p when as RFC 3339 UTC "YYYY-MM-DDTHH:MM:SS+00:00", for the MQTT
 *         last-refresh timestamp sensor (device_class: timestamp).
 *
 * @param  when  a Unix time, or 0 for "never" - writes an empty string in
 *               that case (there is nothing meaningful to format) rather
 *               than the 1970 epoch, which would read as a real timestamp to
 *               Home Assistant.
 */
void ha_time_format_iso8601(time_t when, char *out, size_t len);

/** @brief Convenience for ha_time_format_iso8601(time(NULL), ...); empty if
 *  the clock has never been set. */
void ha_time_iso8601(char *out, size_t len);

#ifdef __cplusplus
}
#endif
