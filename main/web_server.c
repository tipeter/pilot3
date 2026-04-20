#include "web_server.h"
#include "littlefs_mgr.h"
#include "static_handler.h"
#include "system_handler.h"
#include "config_handler.h"
#include "ota_handler.h"
#include "log_ws.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_https_server.h"

static const char *TAG = "WEB_SERVER";

/* Embedded TLS credentials. */
extern const uint8_t server_cert_pem_start[] asm("_binary_server_crt_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_crt_end");
extern const uint8_t server_key_pem_start[]  asm("_binary_server_key_start");
extern const uint8_t server_key_pem_end[]    asm("_binary_server_key_end");

static httpd_handle_t s_server = NULL;

/* ── Route table – SSOT for all URI registrations ───────────────────────── */
/*
 * Every endpoint in this project is registered exactly once, here.
 * Adding a new endpoint means adding one call in this function only.
 * Handlers own their URI strings; this function owns the registration order.
 */
static esp_err_t register_all_handlers(httpd_handle_t server)
{
    esp_err_t err = ESP_OK;

    /* API handlers – registered before the wildcard static handler. */
    err |= system_handler_register(server);
    err |= config_handler_register(server);
    err |= ota_handler_register(server);
    err |= log_ws_register(server);

    /* Static file handler - must be last; the wildcard catches all paths. */
    err |= static_handler_register(server);

    return err;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running.");
        return ESP_OK;
    }

    /* Mount LittleFS before starting the server so static_handler can serve
     * files immediately on the first request. */
    esp_err_t lfs_err = littlefs_mgr_init();
    if (lfs_err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS unavailable – static files will use fallback.");
    }

    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = server_cert_pem_start;
    cfg.servercert_len = (size_t)(server_cert_pem_end - server_cert_pem_start);
    cfg.prvtkey_pem    = server_key_pem_start;
    cfg.prvtkey_len    = (size_t)(server_key_pem_end - server_key_pem_start);

    cfg.port_secure             = CONFIG_PILOT_HTTPS_PORT;
    cfg.httpd.stack_size        = 12288;
    cfg.httpd.max_uri_handlers  = 16;
    /* With TLS session tickets enabled, the browser reuses existing sessions
     * instead of opening new ones. 5 sockets: 1 persistent WS + 4 HTTP. */
    cfg.httpd.max_open_sockets  = 5;
    cfg.httpd.lru_purge_enable  = true;
    cfg.httpd.recv_wait_timeout = 30;
    cfg.httpd.send_wait_timeout = 10;
    /* IDF 6.0: session_tickets is now supported on ESP32-S3.
     * After the first full TLS handshake the server issues a ticket;
     * reconnects skip RSA key exchange and resume in ~50 ms instead of ~1 s. */
    cfg.session_tickets = true;

    /* Enable wildcard URI matching for the static file handler. */
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTPS server on port %d", CONFIG_PILOT_HTTPS_PORT);
    esp_err_t err = httpd_ssl_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        littlefs_mgr_deinit();
        return err;
    }

    err = register_all_handlers(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Handler registration failed – stopping server.");
        httpd_ssl_stop(s_server);
        s_server = NULL;
        littlefs_mgr_deinit();
        return err;
    }

    ESP_LOGI(TAG, "HTTPS server ready.%s",
             littlefs_mgr_is_mounted() ? " LittleFS active." : " LittleFS fallback.");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_err_t err = httpd_ssl_stop(s_server);
    s_server = NULL;
    littlefs_mgr_deinit();
    return err;
}
