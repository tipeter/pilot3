#include "ota_handler.h"
#include "auth.h"
#include "log_ws.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <sys/param.h>

static const char *TAG = "OTA_HANDLER";

#define OTA_BUF_SIZE    CONFIG_PILOT_OTA_BUF_SIZE
#define OTA_MAX_BYTES   CONFIG_PILOT_OTA_MAX_SIZE_BYTES

/* Atomic progress counters: written by the OTA HTTP handler task,
 * read by the OTA status handler task. _Atomic guarantees that
 * concurrent reads on the other core see consistent values. */
static _Atomic uint32_t s_ota_bytes_received = 0;
static _Atomic uint32_t s_ota_total_bytes    = 0;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void broadcast_ota_progress(uint32_t received, uint32_t total)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"ota_progress\",\"bytes\":%"PRIu32",\"total\":%"PRIu32"}",
                     received, total);
    if (n > 0 && n < (int)sizeof(buf)) {
        log_ws_broadcast_json(buf);
    }
}

static void delayed_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ── POST /api/v1/ota ───────────────────────────────────────────────────── */

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    /* Validate Content-Length. */
    if (req->content_len == 0 || req->content_len > OTA_MAX_BYTES) {
        ESP_LOGE(TAG, "Invalid content_len: %zu", req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Content-Length");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA upload started: %zu bytes", req->content_len);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    /* Allocate receive buffer from PSRAM first, fall back to internal SRAM. */
    uint8_t *buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(OTA_BUF_SIZE);
    }
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    /* Reset progress counters atomically before starting. */
    atomic_store(&s_ota_bytes_received, 0);
    atomic_store(&s_ota_total_bytes, (uint32_t)req->content_len);

    size_t remaining = req->content_len;
    bool first_chunk = true;

    while (remaining > 0) {
        int to_read = (int)MIN(remaining, OTA_BUF_SIZE);
        int received = httpd_req_recv(req, (char *)buf, to_read);

        if (received < 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Socket receive error");
            esp_ota_abort(ota_handle);
            free(buf);
            /* Response has not been sent yet; send error. */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Socket error");
            return ESP_FAIL;
        }

        /* Validate the firmware magic byte on the first chunk. */
        if (first_chunk) {
            if (received < 4 || buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware magic byte: 0x%02X", buf[0]);
                esp_ota_abort(ota_handle);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
                return ESP_FAIL;
            }
            first_chunk = false;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write error");
            return ESP_FAIL;
        }

        remaining                -= (size_t)received;
        uint32_t prev = atomic_fetch_add(&s_ota_bytes_received, (uint32_t)received);
        uint32_t curr = prev + (uint32_t)received;
        uint32_t tot  = atomic_load(&s_ota_total_bytes);
        /* Throttle WS broadcasts to ~1% steps to avoid flooding the queue. */
        if (tot > 0) {
            uint32_t prev_pct = (uint32_t)((uint64_t)prev * 100 / tot);
            uint32_t curr_pct = (uint32_t)((uint64_t)curr * 100 / tot);
            if (curr_pct > prev_pct) {
                broadcast_ota_progress(curr, tot);
            }
        }
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        /* ota_handle is already invalidated by esp_ota_end on failure. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA verify failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition error");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA complete. Rebooting in 1.5 s...");
    log_ws_broadcast_json("{\"type\":\"ota_done\",\"status\":\"rebooting\"}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Update accepted. Rebooting.\"}");

    xTaskCreate(delayed_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ── POST /api/v1/ota/rollback ──────────────────────────────────────────── */

static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK || state == ESP_OTA_IMG_INVALID) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"No rollback target available\"}");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Manual rollback requested.");
    log_ws_broadcast_json("{\"type\":\"ota_rollback\",\"status\":\"rebooting\"}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Rolling back. Rebooting.\"}");

    /* Mark the current image invalid and reboot to trigger rollback. */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Not reached. */
    return ESP_OK;
}

/* ── GET /api/v1/ota/status ─────────────────────────────────────────────── */

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    const esp_partition_t   *running  = esp_ota_get_running_partition();
    const esp_app_desc_t    *desc     = esp_app_get_description();
    esp_ota_img_states_t     state    = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version",    desc->version);
    cJSON_AddStringToObject(root, "idf_ver",    desc->idf_ver);
    cJSON_AddStringToObject(root, "date",       desc->date);
    cJSON_AddStringToObject(root, "partition",  running->label);
    cJSON_AddNumberToObject(root, "ota_state",  (double)state);

    /* In-flight progress (non-zero only during an active upload).
     * atomic_load ensures we see a consistent snapshot. */
    uint32_t rx  = atomic_load(&s_ota_bytes_received);
    uint32_t tot = atomic_load(&s_ota_total_bytes);
    if (tot > 0) {
        cJSON_AddNumberToObject(root, "progress_bytes", (double)rx);
        cJSON_AddNumberToObject(root, "progress_total", (double)tot);
        cJSON_AddNumberToObject(root, "progress_pct",   (double)rx * 100.0 / tot);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── URI tables ─────────────────────────────────────────────────────────── */

static const httpd_uri_t s_uris[] = {
    { .uri = "/api/v1/ota",          .method = HTTP_POST, .handler = ota_upload_handler   },
    { .uri = "/api/v1/ota/rollback", .method = HTTP_POST, .handler = ota_rollback_handler  },
    { .uri = "/api/v1/ota/status",   .method = HTTP_GET,  .handler = ota_status_handler    },
};

esp_err_t ota_handler_register(httpd_handle_t server)
{
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        err = httpd_register_uri_handler(server, &s_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s",
                     s_uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
