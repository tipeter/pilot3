#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTPS server and register all URI handlers.
 *
 *        TLS certificate and private key are embedded in flash via
 *        EMBED_TXTFILES (see CMakeLists.txt).
 *
 *        Registers: /, /api/v1/system, /api/v1/config (GET+POST),
 *                   /api/v1/ota, /api/v1/ota/rollback,
 *                   /api/v1/ota/status, /ws/log.
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the HTTPS server (e.g. before entering sleep or on disconnect).
 */
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif
