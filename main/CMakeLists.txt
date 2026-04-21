idf_component_register(
    SRCS "main.c" "wifi_manager.c" "auth.c" "nvs_config.c" "littlefs_mgr.c"
         "web_server.c" "static_handler.c" "system_handler.c" "config_handler.c"
         "ota_handler.c" "log_ws.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_netif esp_event
             esp_https_server esp_http_server esp_partition app_update
             esp_system esp_timer esp_driver_gpio mbedtls littlefs
             network_provisioning
    EMBED_TXTFILES "certs/server.crt" "certs/server.key" "web_ui.html"
)

littlefs_create_partition_image(littlefs ../www FLASH_IN_PROJECT)