#include "config_handler.h"
#include "auth.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "CFG_HANDLER";

/* ── GET /api/v1/config ─────────────────────────────────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    cJSON *obj = NULL;
    if (nvs_config_get_all_json(&obj) != ESP_OK || !obj) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS read error");
        return ESP_FAIL;
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── POST /api/v1/config ────────────────────────────────────────────────── */

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    if (req->content_len == 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *key_item = cJSON_GetObjectItem(root, "key");
    cJSON *val_item = cJSON_GetObjectItem(root, "value");

    if (!cJSON_IsString(key_item) || !key_item->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'key'");
        return ESP_FAIL;
    }

    const char *key = key_item->valuestring;
    esp_err_t   err = ESP_ERR_INVALID_ARG;

    if (cJSON_IsString(val_item)) {
        err = nvs_config_set_str(key, val_item->valuestring);
    } else if (cJSON_IsNumber(val_item)) {
        err = nvs_config_set_i32(key, (int32_t)val_item->valuedouble);
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'value'");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write for key '%s': %s", key, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ── URI table ──────────────────────────────────────────────────────────── */

static const httpd_uri_t s_uris[] = {
    { .uri = "/api/v1/config", .method = HTTP_GET,  .handler = config_get_handler  },
    { .uri = "/api/v1/config", .method = HTTP_POST, .handler = config_post_handler },
};

esp_err_t config_handler_register(httpd_handle_t server)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register '%s': %s",
                     s_uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
