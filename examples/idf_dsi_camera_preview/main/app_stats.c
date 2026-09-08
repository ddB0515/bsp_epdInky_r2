/*
 * Per-core CPU load from the FreeRTOS idle-task run-time counters.
 *
 * Busy time is not measured directly. Instead the idle task's run time is
 * sampled on each call, and whatever is left of the wall-clock interval must
 * have been spent on real work:
 *
 *     busy% = 100 - (idle_delta / elapsed) * 100
 *
 * With CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER the counters are already
 * in microseconds, the same unit as esp_timer_get_time(), so the two can be
 * compared without any scaling.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_stats.h"

static const char *TAG = "app_stats";

static uint32_t s_last_idle[APP_STATS_CORES];
static int64_t  s_last_us;

/* Sum the idle run-time counter of each core's idle task. */
static esp_err_t read_idle(uint32_t idle[APP_STATS_CORES])
{
	memset(idle, 0, sizeof(uint32_t) * APP_STATS_CORES);

	UBaseType_t count = uxTaskGetNumberOfTasks();
	if (count == 0) {
		return ESP_FAIL;
	}

	/* Tasks can be created between the count and the snapshot, so ask for a
	 * few more slots than currently exist. */
	count += 4;
	TaskStatus_t *tasks = calloc(count, sizeof(TaskStatus_t));
	ESP_RETURN_ON_FALSE(tasks, ESP_ERR_NO_MEM, TAG, "no memory for the task list");

	UBaseType_t got = uxTaskGetSystemState(tasks, count, NULL);
	for (UBaseType_t i = 0; i < got; i++) {
		const char *name = tasks[i].pcTaskName;
		if (!name || strncmp(name, "IDLE", 4) != 0) {
			continue;
		}
#if ( configTASKLIST_INCLUDE_COREID == 1 )
		BaseType_t core = tasks[i].xCoreID;
#else
		/* xCoreID is only present when FREERTOS_VTASKLIST_INCLUDE_COREID is
		 * set, and never under the SMP kernel. The idle tasks are named
		 * "IDLE0" and "IDLE1", so the core is readable from the name. */
		BaseType_t core = (name[4] >= '0' && name[4] <= '9') ? (name[4] - '0') : 0;
#endif
		if (core >= 0 && core < APP_STATS_CORES) {
			idle[core] += tasks[i].ulRunTimeCounter;
		}
	}

	free(tasks);
	return ESP_OK;
}

esp_err_t app_stats_init(void)
{
	uint32_t idle[APP_STATS_CORES];
	ESP_RETURN_ON_ERROR(read_idle(idle), TAG, "could not read the idle counters");

	memcpy(s_last_idle, idle, sizeof(idle));
	s_last_us = esp_timer_get_time();
	return ESP_OK;
}

esp_err_t app_stats_cpu(float busy[APP_STATS_CORES])
{
	uint32_t idle[APP_STATS_CORES];
	ESP_RETURN_ON_ERROR(read_idle(idle), TAG, "could not read the idle counters");

	int64_t now     = esp_timer_get_time();
	int64_t elapsed = now - s_last_us;
	if (elapsed < 1000) {
		return ESP_ERR_INVALID_STATE;
	}

	for (int c = 0; c < APP_STATS_CORES; c++) {
		/* Unsigned arithmetic, so a 32-bit counter wrap still subtracts
		 * correctly rather than producing a huge delta. */
		uint32_t delta = idle[c] - s_last_idle[c];
		float    pct   = 100.0f - ((float)delta * 100.0f / (float)elapsed);
		busy[c] = (pct < 0.0f) ? 0.0f : (pct > 100.0f ? 100.0f : pct);
		s_last_idle[c] = idle[c];
	}

	s_last_us = now;
	return ESP_OK;
}
