#include "trmnl_time.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "trmnl_time";

/* Anything before this is the epoch showing through, not a real reading. Set to
 * a date comfortably in the past but well after this firmware was written. */
#define PLAUSIBLE_AFTER ((time_t)1700000000)   /* 2023-11-14 */

/* The RV-3028 is specified at a few ppm, so a day between syncs keeps the error
 * inside a second or two - far tighter than anything here needs. */
#define RESYNC_AFTER_S  (24 * 60 * 60)

#define SNTP_SERVER     "pool.ntp.org"

static rv3028_handle_t s_rtc;
static time_t          s_last_sync;

/* -------------------------------------------------------------------------- */

bool trmnl_time_is_valid(void)
{
    return time(NULL) > PLAUSIBLE_AFTER;
}

void trmnl_time_str(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    if (!trmnl_time_is_valid()) {
        snprintf(out, len, "(no clock)");
        return;
    }

    const time_t now = time(NULL);
    struct tm    tm;
    gmtime_r(&now, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm);
}

bool trmnl_time_sync_due(void)
{
    if (!trmnl_time_is_valid() || s_last_sync == 0) {
        return true;
    }
    return (time(NULL) - s_last_sync) >= RESYNC_AFTER_S;
}

/* -------------------------------------------------------------------------- */

esp_err_t trmnl_time_init(rv3028_handle_t rtc)
{
    s_rtc = rtc;

    if (rtc == NULL) {
        /* The schematic marks the RV-3028 as optional and the BSP downgrades a
         * missing one to a warning, so this is a legitimate configuration. */
        ESP_LOGW(TAG, "no RTC; the clock stays unset until SNTP runs");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t  unix_time = 0;
    esp_err_t err       = rv3028_get_unix_time(rtc, &unix_time);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot read the RTC: %s", esp_err_to_name(err));
        return err;
    }

    if ((time_t)unix_time <= PLAUSIBLE_AFTER) {
        /* A fresh board, or one whose backup cell has run flat. Not an error;
         * the first SNTP sync will set both clocks. */
        ESP_LOGI(TAG, "RTC has never been set");
        return ESP_ERR_INVALID_STATE;
    }

    const struct timeval tv = { .tv_sec = (time_t)unix_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    char now[24];
    trmnl_time_str(now, sizeof(now));
    ESP_LOGI(TAG, "clock seeded from the RTC: %s UTC", now);
    return ESP_OK;
}

esp_err_t trmnl_time_sync_sntp(uint32_t timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));

    /*
     * Always tear down. The SNTP service holds a socket and a timer for a
     * once-a-day job, and leaving it initialised would make the next call fail
     * with ESP_ERR_INVALID_STATE.
     */
    esp_netif_sntp_deinit();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not answer within %" PRIu32 " ms: %s",
                 timeout_ms, esp_err_to_name(err));
        return err;
    }

    s_last_sync = time(NULL);

    char now[24];
    trmnl_time_str(now, sizeof(now));
    ESP_LOGI(TAG, "clock synced: %s UTC", now);

    /* Push it back to the RTC so the next cold boot starts with the right time
     * before the network exists. This is the only reason the RTC is fitted. */
    if (s_rtc != NULL) {
        const esp_err_t wr = rv3028_set_unix_time(s_rtc, (uint32_t)s_last_sync);
        if (wr != ESP_OK) {
            ESP_LOGW(TAG, "cannot write the RTC: %s", esp_err_to_name(wr));
        }
    }

    return ESP_OK;
}
