#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the main application NVS namespace.
 *        Must be called after nvs_flash_init() in app_main.
 */
esp_err_t nvs_config_init(void);

/** @brief Read a string value from the application config namespace. */
esp_err_t nvs_config_get_str(const char *key, char *buf, size_t *len);

/** @brief Write a string value to the application config namespace. */
esp_err_t nvs_config_set_str(const char *key, const char *value);

/** @brief Read an int32 value from the application config namespace. */
esp_err_t nvs_config_get_i32(const char *key, int32_t *out);

/** @brief Write an int32 value to the application config namespace. */
esp_err_t nvs_config_set_i32(const char *key, int32_t value);

/**
 * @brief Serialise all entries in the application config namespace to a
 *        cJSON object.  Caller owns the returned object and must call
 *        cJSON_Delete() on it.
 *
 * @param[out] out  Pointer that receives the newly allocated cJSON object.
 */
esp_err_t nvs_config_get_all_json(cJSON **out);

/**
 * @brief Load a string from the factory NVS partition (read-only).
 *
 * @param key  NVS key to look up.
 * @param buf  Destination buffer.
 * @param len  In: buffer size.  Out: bytes written (including NUL).
 */
esp_err_t nvs_factory_get_str(const char *key, char *buf, size_t *len);

#ifdef __cplusplus
}
#endif
