#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the power manager calls to take the system down and bring it back. */
typedef struct {
	void      (*suspend)(void);  /**< stop servers, release the camera, drop Wi-Fi */
	esp_err_t (*resume)(void);   /**< bring it all back after waking */
} app_power_hooks_t;

/**
 * @brief Start watching for an idle board and sleeping when it is.
 *
 * "Idle" means no MJPEG viewer, no RTSP session, and no HTTP request for
 * @p idle_timeout_s seconds. When that happens the hooks' suspend() runs, the
 * board light-sleeps, and on the wake button it resumes.
 *
 * @param hooks           Suspend/resume callbacks. Must outlive the call.
 * @param idle_timeout_s  Seconds of inactivity before sleeping.
 */
esp_err_t app_power_start(const app_power_hooks_t *hooks, uint32_t idle_timeout_s);

/**
 * @brief Note that something is using the board, restarting the idle countdown.
 *
 * Called from the HTTP handlers, so simply browsing the page keeps the board
 * awake even when nothing is streaming.
 */
void app_power_note_activity(void);

/** @brief Seconds remaining before the board sleeps, or 0 if it is busy. */
uint32_t app_power_idle_seconds_left(void);

/** @brief How many times the board has woken since boot. */
uint32_t app_power_wake_count(void);

/**
 * @brief Why the board last woke, as a short string.
 *
 * Reported over HTTP because the USB console does not survive light sleep on
 * the ESP32-P4, so there is no other way to see it after a nap.
 */
const char *app_power_last_wake_reason(void);

/**
 * @brief Remember which bring-up step failed, across a restart.
 *
 * Stored in RTC memory, because a failed resume restarts the board and the USB
 * console is dead at that point.
 */
void app_power_record_failure(const char *step, esp_err_t err);

/** @brief The step that failed last time, or "" if none. */
const char *app_power_last_failure(esp_err_t *err);

/** @brief Go to sleep now rather than waiting for the countdown. */
void app_power_sleep_now(void);

#ifdef __cplusplus
}
#endif
