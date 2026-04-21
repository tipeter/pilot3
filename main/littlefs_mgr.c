#include "sdkconfig.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <string.h>

#include "littlefs_mgr.h"

static const char *TAG = "LITTLEFS_MGR";

/* ── Module-private state (SSOT) ───────────────────────────────────────── */

static bool s_mounted = false;

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t littlefs_mgr_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = CONFIG_PILOT_LITTLEFS_BASE_PATH,
        .partition_label        = CONFIG_PILOT_LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .dont_mount             = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (%s). "
                 "Run 'idf.py littlefs-flash' to flash the partition.",
                 esp_err_to_name(err));
        return err;
    }

    s_mounted = true;

    size_t total = 0, used = 0;
    littlefs_mgr_stat(&total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at '%s' – used %zu / %zu bytes.",
             CONFIG_PILOT_LITTLEFS_BASE_PATH, used, total);
    return ESP_OK;
}

esp_err_t littlefs_mgr_deinit(void)
{
    if (!s_mounted) return ESP_OK;
    esp_err_t err = esp_vfs_littlefs_unregister(
                        CONFIG_PILOT_LITTLEFS_PARTITION_LABEL);
    if (err == ESP_OK) {
        s_mounted = false;
        ESP_LOGI(TAG, "LittleFS unmounted.");
    }
    return err;
}

bool littlefs_mgr_is_mounted(void)
{
    return s_mounted;
}

const char *littlefs_mgr_base_path(void)
{
    return s_mounted ? CONFIG_PILOT_LITTLEFS_BASE_PATH : NULL;
}

esp_err_t littlefs_mgr_stat(size_t *total_bytes, size_t *used_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    size_t t = 0, u = 0;
    esp_err_t err = esp_littlefs_info(
                        CONFIG_PILOT_LITTLEFS_PARTITION_LABEL, &t, &u);
    if (err == ESP_OK) {
        if (total_bytes) *total_bytes = t;
        if (used_bytes)  *used_bytes  = u;
    }
    return err;
}
