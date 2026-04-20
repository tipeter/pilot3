#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register OTA-related URI handlers with the HTTPS server.
 *
 *  POST /api/v1/ota           – Upload firmware binary (auth required).
 *  POST /api/v1/ota/rollback  – Roll back to the previous partition (auth required).
 *  GET  /api/v1/ota/status    – Current OTA state and firmware info (auth required).
 */
esp_err_t ota_handler_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
