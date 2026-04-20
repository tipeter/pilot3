#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "sdkconfig.h"
#include "nvs_config.h"
#include "auth.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "log_ws.h"

static const char *TAG = "MAIN";

/* ── GPIO ISR reset ─────────────────────────────────────────────────────── */

#define RESET_GPIO      ((gpio_num_t)CONFIG_PILOT_RESET_BUTTON_GPIO)
#define RESET_HOLD_MS   CONFIG_PILOT_RESET_HOLD_TIME_MS

static QueueHandle_t s_reset_q;

static void IRAM_ATTR reset_isr_handler(void *arg)
{
    uint64_t ts = esp_timer_get_time();
    xQueueSendFromISR(s_reset_q, &ts, NULL);
}

static void reset_monitor_task(void *arg)
{
    uint64_t fall_ts;
    while (1) {
        if (xQueueReceive(s_reset_q, &fall_ts, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGW(TAG, "Reset button pressed – hold for %d ms to factory reset.",
                 RESET_HOLD_MS);

        uint32_t held_ms = 0;
        while (gpio_get_level(RESET_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            held_ms += 50;
            if (held_ms >= (uint32_t)RESET_HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset confirmed – erasing credentials.");
                wifi_manager_factory_reset();
            }
        }
        ESP_LOGI(TAG, "Reset button released after %"PRIu32" ms – ignored.", held_ms);
    }
}

static esp_err_t reset_monitor_init(void)
{
    s_reset_q = xQueueCreate(4, sizeof(uint64_t));
    if (!s_reset_q) return ESP_ERR_NO_MEM;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(RESET_GPIO, reset_isr_handler, NULL));

    BaseType_t rc = xTaskCreate(reset_monitor_task, "reset_mon",
                                4096, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_FAIL;
}

/* ── OTA rollback validation ────────────────────────────────────────────── */

static void validate_firmware_on_connect(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   state;

    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Firmware marked stable – rollback cancelled.");
            } else {
                ESP_LOGE(TAG, "Failed to mark firmware stable: %s",
                         esp_err_to_name(err));
            }
        }
    }
}

/* ── Web server start task ──────────────────────────────────────────────── */

/*
 * httpd_ssl_start() performs TLS context initialisation which requires
 * several kilobytes of stack.  The sys_evt task (default event loop task)
 * has only ~3-4 KB of stack, which is not enough.
 *
 * Solution: the Wi-Fi connected callback posts a notification to a
 * dedicated high-priority task that owns sufficient stack and calls
 * web_server_start() there.  The task then suspends itself waiting for
 * the next notification (e.g. after a reconnect cycle).
 */
static TaskHandle_t s_server_task_handle = NULL;

static void web_server_task(void *arg)
{
    while (1) {
        /* Block indefinitely until notified by on_wifi_connected(). */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Starting web server (stack: %u bytes free).",
                 uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        web_server_start();
    }
}

/* ── Wi-Fi callbacks (called from sys_evt context – must be fast) ───────── */

static void on_wifi_connected(void)
{
    validate_firmware_on_connect();

    /* Notify the dedicated server task instead of blocking sys_evt. */
    if (s_server_task_handle) {
        xTaskNotifyGive(s_server_task_handle);
    }
}

static void on_wifi_disconnected(void)
{
    ESP_LOGW(TAG, "Wi-Fi disconnected – waiting for reconnect.");
}

/* ── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 1. NVS flash init ─────────────────────────────────────────────── */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS dirty – erasing and re-initialising.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* ── 2. Application NVS config ─────────────────────────────────────── */
    ESP_ERROR_CHECK(nvs_config_init());

    /* ── 3. Auth subsystem ─────────────────────────────────────────────── */
    ESP_ERROR_CHECK(auth_init());

    /* ── 4. WebSocket log streamer ─────────────────────────────────────── */
    ESP_ERROR_CHECK(log_ws_init());

    /* ── 5. GPIO reset monitor ─────────────────────────────────────────── */
    ESP_ERROR_CHECK(reset_monitor_init());

    /* ── 6. Web server task (large stack for TLS init) ─────────────────── */
    BaseType_t rc = xTaskCreate(web_server_task, "web_srv",
                                16384,   /* 16 KB – TLS + JSON headroom */
                                NULL, 5, &s_server_task_handle);
    ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_FAIL);

    /* ── 7. Wi-Fi manager (provisioning or connect) ────────────────────── */
    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_connected, on_wifi_disconnected));

    ESP_LOGI(TAG, "Initialisation complete. Scheduler running.");
}
