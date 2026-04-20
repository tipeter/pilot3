#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the IP event task when a valid IP address is acquired. */
typedef void (*wifi_connected_cb_t)(void);

/** Called from the Wi-Fi event task on station disconnect. */
typedef void (*wifi_disconnected_cb_t)(void);

/**
 * @brief Initialise Wi-Fi and start BLE provisioning (Security 2 / SRP6a)
 *        if the device has not been provisioned yet.
 *
 *        If already provisioned, connects to the saved AP immediately.
 *        Registers internal event handlers using the instance API.
 *
 * @param on_connect     Called when IP is obtained.  May be NULL.
 * @param on_disconnect  Called on station disconnect.  May be NULL.
 */
esp_err_t wifi_manager_init(wifi_connected_cb_t on_connect,
                            wifi_disconnected_cb_t on_disconnect);

/**
 * @brief Return the unique device ID string (BLE advertisement name and
 *        SRP6a username).  Valid after wifi_manager_init() returns.
 */
const char *wifi_manager_get_device_id(void);

/** @return true if the station currently holds a valid IP address. */
bool wifi_manager_is_connected(void);

/**
 * @brief Copy the current IP address string into buf.
 * @return ESP_ERR_INVALID_STATE if not connected.
 */
esp_err_t wifi_manager_get_ip(char *buf, size_t len);

/**
 * @brief Erase provisioning credentials and restart.
 *        Safe to call from any task.
 */
esp_err_t wifi_manager_factory_reset(void);

#ifdef __cplusplus
}
#endif
