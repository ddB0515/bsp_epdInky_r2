/*
 * Image cache on the micro-SD card.
 *
 * The point is not disk space, it is the two expensive things a cache hit
 * avoids: a TLS download of ~15 KB, and — when the cached file is the one
 * already on the glass — a full GC16 refresh with its four clean cycles. On a
 * device that wakes every 15 minutes to find the same frame, that is most of
 * the energy budget.
 *
 * Upstream keys the cache on the `filename` field of the /api/display response
 * and stores files in SPIFFS. This port uses the SD card instead, by decision:
 * the flash filesystem is small and shared with OTA, while the card is large,
 * removable, and lets a frame be pulled off for inspection.
 *
 * THE CARD IS OPTIONAL. There is no card-detect line on this board (SD1-CD is
 * not routed to the MCU), so an absent or unformatted card shows up as a mount
 * failure and nothing else. That is not an error condition: the firmware simply
 * runs uncached, downloading and refreshing every cycle, and every function
 * here reports "no" so callers need no special case. Do not wire this into the
 * BSP's `use_sdcard` flag, which treats the card as a required device and fails
 * board init without one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "bsp/epdinky_p4_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Mount the card and make sure the cache directory exists.
 *
 * @param  cfg  the same config the board was brought up with; NULL for defaults.
 * @param  tca  the expander handle — SD power is gated through it, so this is
 *              required and the card cannot be reached without it.
 *
 * @return ESP_OK if the cache is usable. Any other value means "running
 *         uncached", which is a normal state, not a fault to report to the user.
 */
esp_err_t trmnl_cache_init(const bsp_epdinky_config_t *cfg, tca6408_handle_t tca);

/**
 * @brief  Flush the card and cut its power.
 *
 * Call before a deep sleep. A no-op when there is no card. After this the cache
 * reports itself unavailable, so anything still running answers "no" rather
 * than touching an unmounted filesystem.
 */
void trmnl_cache_deinit(void);

/** @brief True if a card is mounted and the cache is usable. */
bool trmnl_cache_available(void);

/** @brief True if @p filename is already cached. False when there is no card. */
bool trmnl_cache_has(const char *filename);

/**
 * @brief  Read a cached image into a PSRAM buffer the caller must free with
 *         heap_caps_free().
 *
 * A file that cannot be read is deleted before returning, so a truncated write
 * from an earlier power cut heals itself on the next cycle instead of poisoning
 * the cache permanently.
 */
esp_err_t trmnl_cache_read(const char *filename, uint8_t **out, size_t *out_len);

/**
 * @brief  Store an image under @p filename.
 *
 * Writes to a temporary file and renames, so an interrupted write cannot leave
 * a half-image that trmnl_cache_has() would then report as a hit. Prunes the
 * oldest entries once the directory exceeds its cap.
 */
esp_err_t trmnl_cache_write(const char *filename, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
