#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_config.h"
#include "sdkconfig.h"
/* IDF 6.0: wifi_provisioning removed; use network_provisioning managed component. */
#include "esp_srp.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include <string.h>

#include "wifi_manager.h"

static const char *TAG = "WIFI_MGR";

#define WIFI_CONNECTED_BIT BIT0

static wifi_connected_cb_t    s_on_connect    = NULL;
static wifi_disconnected_cb_t s_on_disconnect = NULL;

static EventGroupHandle_t s_wifi_eg;
static char               s_ip_str[24] = {0};
static bool               s_connected  = false;

static char s_device_id[48]   = {0};

/* SRP6a buffers – int * matches esp_srp_gen_salt_verifier() signature.
 * salt_len is an INPUT to esp_srp_gen_salt_verifier(), not a pointer. */
static char *s_srp_salt     = NULL;
static char *s_srp_verifier = NULL;
static int   s_srp_ver_len  = 0;

#define SRP_SALT_LEN 16

static esp_event_handler_instance_t s_inst_prov;
static esp_event_handler_instance_t s_inst_wifi;
static esp_event_handler_instance_t s_inst_ip;

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void build_device_id(void)
{
    size_t len = sizeof(s_device_id);
    if (nvs_factory_get_str("device_id", s_device_id, &len) == ESP_OK
            && s_device_id[0] != '\0') {
        ESP_LOGI(TAG, "Device ID loaded from factory NVS: %s", s_device_id);
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%s_%02X%02X%02X",
             CONFIG_PILOT_BLE_DEVICE_NAME, mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID derived from MAC: %s", s_device_id);
}

static void free_srp_params(void)
{
    free(s_srp_salt);     s_srp_salt     = NULL;
    free(s_srp_verifier); s_srp_verifier = NULL;
    s_srp_ver_len = 0;
}

/* ── Event handler ──────────────────────────────────────────────────────── */

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    /* IDF 6.0: event base is NETWORK_PROV_EVENT, types are NETWORK_PROV_*.
     * Most function names changed from wifi_prov_mgr_* to network_prov_mgr_*
     * with some exceptions documented in the migration guide. */
    if (base == NETWORK_PROV_EVENT)
    {
      switch (id)
      {
      case NETWORK_PROV_START:
        ESP_LOGI(TAG, "BLE provisioning started.");
        break;
      case NETWORK_PROV_WIFI_CRED_RECV: // ← javítva
        ESP_LOGI(TAG, "Wi-Fi credentials received via BLE.");
        break;
      case NETWORK_PROV_WIFI_CRED_FAIL: {             // ← javítva
        network_prov_wifi_sta_fail_reason_t *reason = // ← javítva
            (network_prov_wifi_sta_fail_reason_t *) data;
        ESP_LOGE(TAG,
                 "Provisioning credential failure: %s",
                 (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) // ← javítva
                     ? "Auth error"
                     : "AP not found");
        network_prov_mgr_reset_wifi_sm_state_on_failure();
        break;
      }
      case NETWORK_PROV_WIFI_CRED_SUCCESS: // ← javítva
        ESP_LOGI(TAG, "Provisioning successful.");
        break;
      case NETWORK_PROV_END:
        network_prov_mgr_deinit();
        free_srp_params();
        ESP_LOGI(TAG, "Provisioning ended; BLE stack released.");
        break;
      }
    }
    else if (base == WIFI_EVENT)
    {
      if (id == WIFI_EVENT_STA_START)
      {
        esp_wifi_connect();
      }
      else if (id == WIFI_EVENT_STA_DISCONNECTED)
      {
        s_connected = false;
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Disconnected. Reconnecting...");
        if (s_on_disconnect)
          s_on_disconnect();
        esp_wifi_connect();
      }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
      ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
      snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
      s_connected = true;
      xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
      ESP_LOGI(TAG, "IP acquired: %s", s_ip_str);
      if (s_on_connect)
        s_on_connect();
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t wifi_manager_init(wifi_connected_cb_t on_connect,
                            wifi_disconnected_cb_t on_disconnect)
{
    s_on_connect    = on_connect;
    s_on_disconnect = on_disconnect;

    s_wifi_eg = xEventGroupCreate();
    if (!s_wifi_eg) return ESP_ERR_NO_MEM;

    build_device_id();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, &s_inst_prov));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, &s_inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, &s_inst_ip));

    /* IDF 6.0: network_prov_mgr_config_t replaces wifi_prov_mgr_config_t.
     * scheme field: network_prov_scheme_ble replaces wifi_prov_scheme_ble. */
    network_prov_mgr_config_t prov_cfg = {
        .scheme               = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    /* IDF 6.0 rename: wifi_prov_mgr_is_provisioned
     *              -> network_prov_mgr_is_wifi_provisioned */
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        char pop[48] = {0};
        size_t pop_len = sizeof(pop);
        esp_err_t err = nvs_factory_get_str("device_password", pop, &pop_len);
        if (err != ESP_OK || pop[0] == '\0') {
            ESP_LOGW(TAG, "Factory device_password unavailable – using compile-time fallback.");
            strncpy(pop, CONFIG_PILOT_FALLBACK_POP_CODE, sizeof(pop) - 1);
        }

        /* Derive SRP6a verifier: v = g^H(salt||H(username:password)) mod N
         * username (I) = unique device ID; password (P) = device_password. */
        err = esp_srp_gen_salt_verifier(s_device_id,    (int)strlen(s_device_id),
                                        pop,             (int)strlen(pop),
                                        &s_srp_salt,     SRP_SALT_LEN,
                                        &s_srp_verifier, &s_srp_ver_len);
        explicit_bzero(pop, sizeof(pop));

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SRP6a generation failed: %s", esp_err_to_name(err));
            return err;
        }

        /* IDF 6.0: network_prov_security2_params_t replaces
         *          wifi_prov_security2_params_t. */
        network_prov_security2_params_t sec2 = {
            .salt         = s_srp_salt,
            .salt_len     = SRP_SALT_LEN,
            .verifier     = s_srp_verifier,
            .verifier_len = (uint16_t)s_srp_ver_len,
        };

        ESP_LOGI(TAG, "Starting BLE provisioning (Security 2 / SRP6a) as '%s'",
                 s_device_id);
        /* IDF 6.0: NETWORK_PROV_SECURITY_2 replaces WIFI_PROV_SECURITY_2. */
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_2, (const void *)&sec2,
            s_device_id, NULL));

    } else {
        ESP_LOGI(TAG, "Already provisioned. Connecting to saved AP...");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    return ESP_OK;
}

const char *wifi_manager_get_device_id(void)
{
    return s_device_id;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_manager_get_ip(char *buf, size_t len)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    snprintf(buf, len, "%s", s_ip_str);
    return ESP_OK;
}

esp_err_t wifi_manager_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing provisioning data.");
    /* IDF 6.0 rename: wifi_prov_mgr_reset_provisioning
     *              -> network_prov_mgr_reset_wifi_provisioning */
    network_prov_mgr_reset_wifi_provisioning();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
