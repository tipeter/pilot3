#include "system_handler.h"
#include "auth.h"
#include "littlefs_mgr.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <inttypes.h>

static const char *TAG = "SYS_HANDLER";

static esp_err_t system_get_handler(httpd_req_t *req)
{
    if (!auth_check_bearer(req)) return ESP_FAIL;

    const esp_app_desc_t *desc = esp_app_get_description();

    /* Wi-Fi info. */
    char    ip_str[24] = "N/A";
    int8_t  rssi       = 0;
    esp_netif_t *sta   = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
        }
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    uint64_t uptime_s = (uint64_t)(esp_timer_get_time() / 1000000ULL);

    /* LittleFS usage (optional – zero if not mounted). */
    size_t lfs_total = 0, lfs_used = 0;
    if (littlefs_mgr_is_mounted()) {
        littlefs_mgr_stat(&lfs_total, &lfs_used);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "fw_version",   desc->version);
    cJSON_AddStringToObject(root, "idf_version",  desc->idf_ver);
    cJSON_AddStringToObject(root, "compile_date", desc->date);
    cJSON_AddStringToObject(root, "ip",           ip_str);
    cJSON_AddNumberToObject(root, "rssi_dbm",     (double)rssi);
    cJSON_AddNumberToObject(root, "uptime_s",     (double)uptime_s);
    cJSON_AddNumberToObject(root, "free_heap",    (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_heap",     (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram",   (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "lfs_total",    (double)lfs_total);
    cJSON_AddNumberToObject(root, "lfs_used",     (double)lfs_used);
    cJSON_AddBoolToObject  (root, "lfs_mounted",  littlefs_mgr_is_mounted());

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

static const httpd_uri_t s_uri = {
    .uri     = "/api/v1/system",
    .method  = HTTP_GET,
    .handler = system_get_handler,
};

esp_err_t system_handler_register(httpd_handle_t server)
{
    esp_err_t err = httpd_register_uri_handler(server, &s_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register: %s", esp_err_to_name(err));
    }
    return err;
}
