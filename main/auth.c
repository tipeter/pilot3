#include "auth.h"
#include "nvs_config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "AUTH";

/* NVS key under which the API token is stored in the app namespace. */
#define TOKEN_NVS_KEY "api_token"

/* Fixed-length token: 32 hex chars = 16 random bytes. */
#define TOKEN_LEN 32

/* In-memory copy of the active token – zero-initialised. */
static char s_token[TOKEN_LEN + 1] = {0};

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void generate_random_token(char *buf, size_t len)
{
    /* len must be TOKEN_LEN + 1 (NUL included). */
    uint8_t raw[TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(buf + i * 2, 3, "%02x", raw[i]);
    }
    buf[TOKEN_LEN] = '\0';
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t auth_init(void)
{
    /* 1. Try factory NVS first. */
    size_t len = sizeof(s_token);
    esp_err_t err = nvs_factory_get_str(TOKEN_NVS_KEY, s_token, &len);
    if (err == ESP_OK && strnlen(s_token, TOKEN_LEN + 1) >= 8) {
        ESP_LOGI(TAG, "API token loaded from factory NVS.");
        return ESP_OK;
    }

    /* 2. Try application NVS (persisted from a previous boot). */
    len = sizeof(s_token);
    err = nvs_config_get_str(TOKEN_NVS_KEY, s_token, &len);
    if (err == ESP_OK && strnlen(s_token, TOKEN_LEN + 1) >= 8) {
        ESP_LOGI(TAG, "API token loaded from application NVS.");
        return ESP_OK;
    }

    /* 3. Generate a fresh token and persist it. */
    generate_random_token(s_token, sizeof(s_token));
    err = nvs_config_set_str(TOKEN_NVS_KEY, s_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist generated token: %s", esp_err_to_name(err));
        /* Not fatal – the token is still valid for this boot. */
    }

    /* Log prominently so the operator can retrieve it from the console. */
    ESP_LOGW(TAG, "╔══════════════════════════════════════════════════╗");
    ESP_LOGW(TAG, "║  NEW API TOKEN GENERATED – NOTE THIS DOWN        ║");
    ESP_LOGW(TAG, "║  Token: %-40s ║", s_token);
    ESP_LOGW(TAG, "╚══════════════════════════════════════════════════╝");

    return ESP_OK;
}

bool auth_validate_token(const char *token)
{
    if (!token || s_token[0] == '\0') return false;
    /* Constant-time comparison to prevent timing attacks. */
    size_t a_len = strnlen(s_token, TOKEN_LEN + 1);
    size_t b_len = strnlen(token,   TOKEN_LEN + 1);
    if (a_len != b_len) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a_len; i++) {
        diff |= (uint8_t)(s_token[i] ^ token[i]);
    }
    return (diff == 0);
}

bool auth_check_bearer(httpd_req_t *req)
{
    /* Retrieve the Authorization header. */
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) {
        ESP_LOGW(TAG, "Missing Authorization header from %s", req->uri);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"ESP32-S3 Pilot\"");
        httpd_resp_sendstr(req, "{\"error\":\"Unauthorized\"}");
        return false;
    }

    /* hdr_len does not include the NUL terminator. */
    char *hdr = malloc(hdr_len + 1);
    if (!hdr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return false;
    }

    httpd_req_get_hdr_value_str(req, "Authorization", hdr, hdr_len + 1);

    /* Expect "Bearer <token>" */
    const char *prefix = "Bearer ";
    const size_t prefix_len = strlen(prefix);
    bool valid = false;

    if (strncmp(hdr, prefix, prefix_len) == 0) {
        valid = auth_validate_token(hdr + prefix_len);
    }

    free(hdr);

    if (!valid) {
        ESP_LOGW(TAG, "Invalid token for %s", req->uri);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"ESP32-S3 Pilot\"");
        httpd_resp_sendstr(req, "{\"error\":\"Forbidden\"}");
    }
    return valid;
}
