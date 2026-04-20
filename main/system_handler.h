#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file system_handler.h
 * @brief Single responsibility: system telemetry REST endpoint.
 *
 * Endpoint: GET /api/v1/system
 * Response: JSON with heap, PSRAM, uptime, IP, RSSI, firmware version,
 *           LittleFS usage.
 * Auth: Bearer token required.
 */
esp_err_t system_handler_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
