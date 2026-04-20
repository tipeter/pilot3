#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file config_handler.h
 * @brief Single responsibility: device configuration REST endpoints.
 *
 * Endpoints:
 *   GET  /api/v1/config  – returns all NVS config keys as JSON.
 *   POST /api/v1/config  – sets one NVS key. Body: {"key":"k","value":"v"|n}
 * Auth: Bearer token required on both endpoints.
 */
esp_err_t config_handler_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
