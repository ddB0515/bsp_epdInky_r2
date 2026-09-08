/*
 * Persistence — the NVS store behind everything that has to survive a reboot.
 *
 * One NVS namespace (HA_NVS_NAMESPACE), holding whatever the provisioning
 * portal collected: Wi-Fi credentials, the Home Assistant WebSocket
 * host/port and long-lived access token. Nothing here is compiled in - see
 * ha_config.h's header comment.
 *
 * Reads take a fallback rather than reporting "missing" as an error: almost
 * every caller wants a default on first boot. Where the distinction matters -
 * "does this key exist at all" - use ha_persist_exists().
 *
 * Identical to examples/idf_epd_ha_firmware/main/ha_persist.h - this module
 * has no MQTT/e-paper-specific content, so it's reused verbatim.
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
esp_err_t ha_persist_init(void);

/** @brief True if @p key has a stored value of any type. */
bool ha_persist_exists(const char *key);

/**
 * @brief  Read a string, or leave @p out empty if the key is absent.
 *
 * @return ESP_OK whether or not the key existed; an error only if NVS itself
 *         failed or @p out was too small.
 */
esp_err_t ha_persist_get_str(const char *key, char *out, size_t len);

/** @brief Write a string, committing immediately. */
esp_err_t ha_persist_set_str(const char *key, const char *value);

/** @brief Read an unsigned value, returning @p fallback if the key is absent. */
uint32_t ha_persist_get_u32(const char *key, uint32_t fallback);

/** @brief Write an unsigned value, committing immediately. */
esp_err_t ha_persist_set_u32(const char *key, uint32_t value);

/** @brief Remove a key. Absent keys are not an error. */
esp_err_t ha_persist_erase(const char *key);

#ifdef __cplusplus
}
#endif
