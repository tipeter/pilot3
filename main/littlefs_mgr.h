#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file littlefs_mgr.h
 * @brief Single owner of the LittleFS filesystem lifecycle and mount state.
 *
 * SSOT responsibilities:
 *  - Filesystem mount / unmount state (no other module checks this directly).
 *  - Base path string (all modules that need it call littlefs_mgr_base_path()).
 *  - Partition label (sourced from Kconfig, never repeated elsewhere).
 *
 * Other modules must NEVER call esp_littlefs_* directly; they use this API.
 */

/**
 * @brief Mount the LittleFS partition and register it with the VFS.
 *
 *        Idempotent: safe to call multiple times; returns ESP_OK if already
 *        mounted.
 *
 * @return ESP_OK on success, or an esp_err_t describing the failure.
 */
esp_err_t littlefs_mgr_init(void);

/**
 * @brief Unmount the LittleFS partition and unregister from the VFS.
 *        No-op if not mounted.
 */
esp_err_t littlefs_mgr_deinit(void);

/**
 * @brief Query mount state.
 * @return true  if the filesystem is currently mounted and usable.
 * @return false otherwise.
 */
bool littlefs_mgr_is_mounted(void);

/**
 * @brief Return the VFS base path (e.g. "/www").
 *
 *        The returned pointer is valid for the lifetime of the process.
 *        Returns NULL if not mounted.
 */
const char *littlefs_mgr_base_path(void);

/**
 * @brief Fill *total_bytes and *used_bytes with filesystem usage statistics.
 *        Both output parameters may be NULL if the caller does not need them.
 *
 * @return ESP_ERR_INVALID_STATE if not mounted.
 */
esp_err_t littlefs_mgr_stat(size_t *total_bytes, size_t *used_bytes);

#ifdef __cplusplus
}
#endif
