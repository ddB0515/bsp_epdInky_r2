#include "trmnl_refresh.h"

#include <inttypes.h>

#include "esp_log.h"

#include "trmnl_config.h"
#include "trmnl_persist.h"

static const char *TAG = "trmnl_refresh";

/* Upstream's RefreshInterval::DEFAULT_SECONDS. Same value as
 * TRMNL_SLEEP_TIME_DEFAULT_S, which is where it comes from. */
#define DEFAULT_SECONDS TRMNL_SLEEP_TIME_DEFAULT_S

static uint32_t store(uint32_t seconds)
{
    if (trmnl_persist_set_u32_if_changed(TRMNL_NVS_REFRESH_RATE, seconds)) {
        ESP_LOGI(TAG, "refresh interval is now %" PRIu32 " s", seconds);
    }
    return seconds;
}

uint32_t trmnl_refresh_seconds(void)
{
    return trmnl_persist_get_u32(TRMNL_NVS_REFRESH_RATE, DEFAULT_SECONDS);
}

uint32_t trmnl_refresh_apply_server_rate(uint32_t rate)
{
    /*
     * Stored exactly as sent, matching upstream: applyServerRate() does no
     * clamping, and TRMNL_SLEEP_TIME_MIN_S / _MAX_S exist to describe what the
     * server is expected to send rather than to police it. A server that asks
     * for something absurd is a server-side bug worth seeing in the log, not
     * one worth silently papering over here.
     */
    return store(rate);
}

uint32_t trmnl_refresh_apply_api_retry(uint8_t attempt)
{
    uint32_t seconds;
    switch (attempt) {
    case 1:  seconds = 15;              break;
    case 2:  seconds = 30;              break;
    case 3:  seconds = 60;              break;
    default: seconds = DEFAULT_SECONDS; break;
    }
    return store(seconds);
}

uint32_t trmnl_refresh_apply_wifi_retry(uint8_t attempt)
{
    uint32_t seconds;
    switch (attempt) {
    case 1:  seconds = 60;  break;
    case 2:  seconds = 180; break;
    default: seconds = 300; break;
    }
    return store(seconds);
}

/** Upstream's fastPollSeconds(): quick at first, then decaying in three steps. */
static uint32_t fast_poll_seconds(uint32_t streak)
{
    if (streak <= 50u) { return 5;    }
    if (streak <= 60u) { return 60;   }
    if (streak <= 70u) { return 900;  }
    return 3600;
}

uint32_t trmnl_refresh_apply_fast_poll(void)
{
    const uint32_t streak = trmnl_persist_get_u32(TRMNL_NVS_FAST_POLLS, 0) + 1u;
    trmnl_persist_set_u32(TRMNL_NVS_FAST_POLLS, streak);
    return store(fast_poll_seconds(streak));
}

uint32_t trmnl_refresh_apply_default(void)
{
    return store(DEFAULT_SECONDS);
}

void trmnl_refresh_reset_fast_poll_streak(void)
{
    /* Guarded so the common path - a device showing a real image every cycle -
     * does not write to flash 96 times a day for no reason. */
    if (trmnl_persist_get_u32(TRMNL_NVS_FAST_POLLS, 0) != 0u) {
        trmnl_persist_set_u32(TRMNL_NVS_FAST_POLLS, 0);
    }
}
