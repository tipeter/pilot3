#include "web_server.h"
#include "littlefs_mgr.h"
#include "static_handler.h"
#include "system_handler.h"
#include "config_handler.h"
#include "ota_handler.h"
#include "log_ws.h"
#include "cert_mgr.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_https_server.h"

static const char *TAG = "WEB_SERVER";

static httpd_handle_t s_server = NULL;

static esp_err_t register_all_handlers(httpd_handle_t server)
{
  esp_err_t err = ESP_OK;
  err |= system_handler_register(server);
  err |= config_handler_register(server);
  err |= ota_handler_register(server);
  err |= log_ws_register(server);
  err |= static_handler_register(server);
  return err;
}

esp_err_t web_server_start(void)
{
  if (s_server)
    return ESP_OK;

  esp_err_t lfs_err = littlefs_mgr_init();
  if (lfs_err != ESP_OK)
  {
    ESP_LOGW(TAG, "LittleFS unavailable – static files will use fallback.");
  }

  httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();

  size_t cert_len, key_len;
  cfg.servercert = cert_mgr_get_cert_pem(&cert_len);
  cfg.servercert_len = cert_len - 1;
  cfg.prvtkey_pem = cert_mgr_get_key_pem(&key_len);
  cfg.prvtkey_len = key_len - 1;

  cfg.port_secure = CONFIG_PILOT_HTTPS_PORT;
  cfg.httpd.stack_size = 12288;
  cfg.httpd.max_uri_handlers = 16;

  /* Párhuzamos lekérések biztosítása */
  cfg.httpd.max_open_sockets = 8;
  cfg.httpd.lru_purge_enable = true;
  cfg.httpd.recv_wait_timeout = 30;
  cfg.httpd.send_wait_timeout = 10;

  /* BÖNGÉSZŐ HIBA JAVÍTÁSA: Session tickets kikapcsolva */
  cfg.session_tickets = false;

  cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "Starting HTTPS server on port %d", CONFIG_PILOT_HTTPS_PORT);
  esp_err_t err = httpd_ssl_start(&s_server, &cfg);

  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
    littlefs_mgr_deinit();
    return err;
  }

  if (register_all_handlers(s_server) != ESP_OK)
  {
    httpd_ssl_stop(s_server);
    s_server = NULL;
    littlefs_mgr_deinit();
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "HTTPS server ready.%s",
           littlefs_mgr_is_mounted() ? " LittleFS active."
                                     : " LittleFS fallback.");

  return ESP_OK;
}

esp_err_t web_server_stop(void)
{
  if (!s_server)
    return ESP_OK;
  esp_err_t err = httpd_ssl_stop(s_server);
  s_server = NULL;
  littlefs_mgr_deinit();
  return err;
}