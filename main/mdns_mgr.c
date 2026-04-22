#include "mdns_mgr.h"
#include "cert_mgr.h"
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "MDNS_MGR";

esp_err_t mdns_mgr_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    char hostname[64] = "pilot"; /* Fallback */
    if (cert_mgr_get_hostname(hostname, sizeof(hostname)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to extract hostname from cert, using fallback: %s", hostname);
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set("Pilot ESP32-S3 Server");
    
    /* Hardcoded port 443 can be read from sdkconfig if preferred */
    mdns_service_add("Pilot-Web", "_https", "_tcp", 443, NULL, 0);

    ESP_LOGI(TAG, "mDNS service active. Reachable at https://%s.local", hostname);
    return ESP_OK;
}
