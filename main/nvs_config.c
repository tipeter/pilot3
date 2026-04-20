#include "nvs_config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "NVS_CONFIG";

#define APP_NVS_NAMESPACE        "app_cfg"
#define FACTORY_PARTITION        CONFIG_PILOT_FACTORY_PARTITION_NAME
#define FACTORY_NAMESPACE        CONFIG_PILOT_FACTORY_NVS_NAMESPACE

/* ── Internal helpers ───────────────────────────────────────────────────── */

static esp_err_t open_app_nvs(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(APP_NVS_NAMESPACE, mode, out);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t nvs_config_init(void)
{
    /* Verify that the namespace is accessible; creates it if absent. */
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_close(h);
    }
    return err;
}

esp_err_t nvs_config_get_str(const char *key, char *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, buf, len);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_get_i32(const char *key, int32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_i32(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_config_get_all_json(cJSON **out)
{
    nvs_handle_t h;
    esp_err_t err = open_app_nvs(NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) { nvs_close(h); return ESP_ERR_NO_MEM; }

    nvs_iterator_t it = NULL;
    err = nvs_entry_find(NVS_DEFAULT_PART_NAME, APP_NVS_NAMESPACE, NVS_TYPE_ANY, &it);

    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        switch (info.type) {
            case NVS_TYPE_STR: {
                size_t len = 0;
                if (nvs_get_str(h, info.key, NULL, &len) == ESP_OK) {
                    char *val = malloc(len);
                    if (val && nvs_get_str(h, info.key, val, &len) == ESP_OK) {
                        cJSON_AddStringToObject(obj, info.key, val);
                    }
                    free(val);
                }
                break;
            }
            case NVS_TYPE_I32: {
                int32_t val;
                if (nvs_get_i32(h, info.key, &val) == ESP_OK) {
                    cJSON_AddNumberToObject(obj, info.key, (double)val);
                }
                break;
            }
            default:
                break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);

    *out = obj;
    return ESP_OK;
}

esp_err_t nvs_factory_get_str(const char *key, char *buf, size_t *len)
{
    /* Factory partition is read-only by design. */
    esp_err_t err = nvs_flash_init_partition(FACTORY_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Factory partition '%s' init failed: %s",
                 FACTORY_PARTITION, esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(FACTORY_PARTITION, FACTORY_NAMESPACE,
                                  NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Factory NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_get_str(h, key, buf, len);
    nvs_close(h);
    return err;
}
