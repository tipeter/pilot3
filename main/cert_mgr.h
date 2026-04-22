#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* * Fetches the embedded PEM certificate and its length. 
 * Length includes the null terminator required by mbedtls.
 */
const uint8_t* cert_mgr_get_cert_pem(size_t *out_len);

/* Fetches the embedded PEM private key and its length. */
const uint8_t* cert_mgr_get_key_pem(size_t *out_len);

/* * Parses the embedded certificate and extracts the Common Name (CN).
 * Strips the ".local" suffix if present.
 */
esp_err_t cert_mgr_get_hostname(char *out_hostname, size_t max_len);
