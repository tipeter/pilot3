#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the WebSocket log subsystem.
 *
 *        Creates the internal message queue and installs a custom
 *        vprintf handler that forwards every log line to the queue
 *        (non-blocking – messages are dropped if the queue is full).
 *
 *        Must be called before web_server_start().
 */
esp_err_t log_ws_init(void);

/**
 * @brief Register the WebSocket endpoint with an already-started server.
 *
 *        Endpoint: GET /ws/log  (upgraded to WebSocket)
 *        Authentication: ?token=<api_token> query parameter.
 *
 * @param server  Handle returned by httpd_ssl_start() / httpd_start().
 */
esp_err_t log_ws_register(httpd_handle_t server);

/**
 * @brief Enqueue an arbitrary JSON-formatted message for broadcast.
 *
 *        Can be called from any task (including the OTA handler) to
 *        push structured events to all connected WebSocket clients.
 *
 * @param json_str  NUL-terminated JSON string.  Truncated to
 *                  CONFIG_PILOT_LOG_WS_MSG_MAX_LEN if longer.
 */
void log_ws_broadcast_json(const char *json_str);

#ifdef __cplusplus
}
#endif
