/*
 * Persistence — the NVS store behind everything that has to survive a reboot.
 *
 * This is upstream's `Persistence` interface (include/persistence_interface.h)
 * written as a C API. Upstream needs it as an abstract class so the unit tests
 * can substitute an in-memory implementation; here it is a plain module over
 * one NVS namespace, and the indirection buys nothing.
 *
 * Keys are the ones in trmnl_config.h, which are byte-for-byte upstream's, so a
 * device provisioned by either firmware is readable by the other.
 *
 * Reads take a fallback rather than reporting "missing" as an error: almost
 * every caller wants a default on first boot, and upstream's `readUint(key,
 * defaultValue)` has the same shape. Where the distinction matters —
 * `writeIfChanged` needs to know whether a record exists at all — use
 * trmnl_persist_exists().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Open the NVS namespace.
 *
 * Initialises NVS itself if nobody has yet, and re-initialises it after an
 * ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND erase. Safe to call twice.
 */
esp_err_t trmnl_persist_init(void);

/** @brief True if @p key has a stored value of any type. */
bool trmnl_persist_exists(const char *key);

/**
 * @brief  Read a string, or leave @p out empty if the key is absent.
 *
 * @return ESP_OK whether or not the key existed; an error only if NVS itself
 *         failed or @p out was too small.
 */
esp_err_t trmnl_persist_get_str(const char *key, char *out, size_t len);

/** @brief Write a string, committing immediately. */
esp_err_t trmnl_persist_set_str(const char *key, const char *value);

/** @brief Read an unsigned value, returning @p fallback if the key is absent. */
uint32_t trmnl_persist_get_u32(const char *key, uint32_t fallback);

/** @brief Write an unsigned value, committing immediately. */
esp_err_t trmnl_persist_set_u32(const char *key, uint32_t value);

/**
 * @brief  Write only if the stored value differs.
 *
 * Upstream's `writeIfChanged()`. Flash endurance is the point: the refresh
 * interval is re-applied on every cycle and is usually unchanged.
 *
 * @return true if a write actually happened.
 */
bool trmnl_persist_set_u32_if_changed(const char *key, uint32_t value);

/** @brief Remove a key. Absent keys are not an error. */
esp_err_t trmnl_persist_erase(const char *key);

#ifdef __cplusplus
}
#endif
