#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the API token from factory NVS (key "api_token").
 *        If absent, generates a random 32-char hex token, stores it in
 *        the main application NVS and logs it to UART so the operator
 *        can retrieve it from the serial console.
 *
 *        Must be called after nvs_config_init().
 */
esp_err_t auth_init(void);

/**
 * @brief Validate the Bearer token in an HTTP request's Authorization header.
 *
 * @param req  Incoming HTTP request.
 * @return true   Token is valid.
 * @return false  Token is missing or invalid; the handler should return ESP_FAIL
 *                immediately (a 401 response has already been sent).
 */
bool auth_check_bearer(httpd_req_t *req);

/**
 * @brief Validate a token string directly (used for WebSocket query-param auth).
 *
 * @param token  NUL-terminated token string to validate.
 * @return true  if the token matches the stored value.
 */
bool auth_validate_token(const char *token);

#ifdef __cplusplus
}
#endif
