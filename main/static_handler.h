#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file static_handler.h
 * @brief Single owner of static asset serving and the MIME type table.
 *
 * SSOT responsibilities:
 *  - MIME type mapping (defined once in static_handler.c, nowhere else).
 *  - Serving logic: LittleFS first, embedded fallback for "/".
 *  - ETag generation and cache control headers.
 *  - Pre-compressed (.gz) variant selection.
 *
 * Endpoint registered: GET /  and  GET /wildcard
 */

/**
 * @brief Register the static file handler with the HTTPS server.
 *
 *        When the LittleFS filesystem is mounted:
 *          GET /         → serves BASE_PATH/index.html
 *          GET /app.js   → serves BASE_PATH/app.js
 *          GET /any      → serves BASE_PATH/<uri>
 *          For each file, checks for a pre-compressed BASE_PATH/<file>.gz first.
 *
 *        Fallback (LittleFS not mounted or file not found for "/"):
 *          GET /         → serves the embedded web_ui.html (EMBED_TXTFILES).
 *
 * @param server  Handle of the running HTTPS server.
 */
esp_err_t static_handler_register(httpd_handle_t server);

/**
 * @brief Look up the MIME type string for a file extension.
 *
 *        This is the project-wide SSOT for MIME types.  Other modules
 *        that need MIME strings (e.g. a future download endpoint) must
 *        call this function rather than defining their own mapping.
 *
 * @param filename  Full filename or path (e.g. "index.html", "/app.js").
 * @return          Pointer to a static MIME string, or "application/octet-stream"
 *                  if the extension is unknown.
 */
const char *static_handler_mime(const char *filename);

#ifdef __cplusplus
}
#endif
