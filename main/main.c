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
#include "esp_task_wdt.h"

#include "sdkconfig.h"
#include "nvs_config.h"
#include "auth.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "log_ws.h"

static const char *TAG = "MAIN";

/* ── GPIO ISR reset ─────────────────────────────────────────────────────── */

#define RESET_GPIO ((gpio_num_t) CONFIG_PILOT_RESET_BUTTON_GPIO)
#define RESET_HOLD_MS CONFIG_PILOT_RESET_HOLD_TIME_MS

static QueueHandle_t s_reset_q;

static void IRAM_ATTR reset_isr_handler(void *arg)
{
  uint64_t ts = esp_timer_get_time();
  xQueueSendFromISR(s_reset_q, &ts, NULL);
}

static void reset_monitor_task(void *arg)
{
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  uint64_t fall_ts;
  while (1)
  {
    esp_task_wdt_reset();

    if (xQueueReceive(s_reset_q, &fall_ts, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
      continue;
    }

    ESP_LOGW(TAG,
             "Reset button pressed – hold for %d ms to factory reset.",
             RESET_HOLD_MS);

    uint32_t held_ms = 0;
    while (gpio_get_level(RESET_GPIO) == 0)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
      held_ms += 50;
      if (held_ms >= (uint32_t) RESET_HOLD_MS)
      {
        ESP_LOGW(TAG, "Factory reset confirmed – erasing credentials.");
        wifi_manager_factory_reset();
      }
    }
    ESP_LOGI(TAG,
             "Reset button released after %" PRIu32 " ms – ignored.",
             held_ms);
  }
}

static esp_err_t reset_monitor_init(void)
{
  s_reset_q = xQueueCreate(4, sizeof(uint64_t));
  if (!s_reset_q)
    return ESP_ERR_NO_MEM;

  gpio_config_t cfg = {
      .pin_bit_mask = (1ULL << RESET_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(RESET_GPIO, reset_isr_handler, NULL));

  BaseType_t rc
      = xTaskCreate(reset_monitor_task, "reset_mon", 4096, NULL, 5, NULL);
  return (rc == pdPASS) ? ESP_OK : ESP_FAIL;
}

/* ── OTA rollback validation ────────────────────────────────────────────── */

static void validate_firmware_on_connect(void)
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;

  if (esp_ota_get_state_partition(running, &state) == ESP_OK)
  {
    if (state == ESP_OTA_IMG_PENDING_VERIFY)
    {
      esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err == ESP_OK)
      {
        ESP_LOGI(TAG, "Firmware marked stable – rollback cancelled.");
      }
      else
      {
        ESP_LOGE(TAG,
                 "Failed to mark firmware stable: %s",
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
  /* Subscribe to TWDT once */
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  ESP_LOGI(TAG,
           "web_server_task (web_srv) started (stack high-water: %u bytes)",
           uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));

  /* One immediate reset right after subscription */
  esp_task_wdt_reset();

  while (1)
  {
    esp_task_wdt_reset(); /* feed every loop – critical */

    /* Wait max 5 seconds for Wi-Fi connected notification */
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

    if (notified)
    {
      ESP_LOGI(TAG, "Wi-Fi connected – starting HTTPS server...");
      web_server_start();
    }
    /* else: still in provisioning or waiting → just continue feeding TWDT */
  }
}

/* ── Wi-Fi callbacks (called from sys_evt context – must be fast) ───────── */

static void on_wifi_connected(void)
{
  validate_firmware_on_connect();

  /* Notify the dedicated server task instead of blocking sys_evt. */
  if (s_server_task_handle)
  {
    xTaskNotifyGive(s_server_task_handle);
  }
}

static void on_wifi_disconnected(void)
{
  ESP_LOGW(TAG, "Wi-Fi disconnected – waiting for reconnect.");
}

static void heap_monitor_task(void *pvParameters)
{
  ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  while (1)
  {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI("HEAP",
             "Free RAM: %u KiB | PSRAM: %u KiB | Largest PSRAM block: %u KiB",
             heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
  }
}

/* ── app_main ───────────────────────────────────────────────────────────── */

void app_main(void)
{
#if CONFIG_ESP_TASK_WDT_INIT
  esp_task_wdt_deinit();
#endif // CONFIG_ESP_TASK_WDT_INIT

  /* Initialise Task Watchdog (10s timeout, panic on trigger) */
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 10000, /* 10 seconds – enough for WiFi/HTTPS/WS operations */
      .trigger_panic = true, /* panic + reset on timeout (production default) */
      .idle_core_mask = (1U << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
  };
  ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
  ESP_LOGI(TAG, "Task Watchdog Timer initialised (10s timeout, panic enabled)");

  /* ── 1. NVS flash init ─────────────────────────────────────────────── */
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
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

  xTaskCreatePinnedToCore(heap_monitor_task, "heap_mon", 3072, NULL, 3, NULL, 1);

  /* ── 6. Web server task (large stack for TLS init) ─────────────────── */
  BaseType_t rc = xTaskCreate(web_server_task,
                              "web_srv",
                              16384, /* 16 KB – TLS + JSON headroom */
                              NULL,
                              5,
                              &s_server_task_handle);
  ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_FAIL);

  /* ── 7. Wi-Fi manager (provisioning or connect) ────────────────────── */
  ESP_ERROR_CHECK(wifi_manager_init(on_wifi_connected, on_wifi_disconnected));

  ESP_LOGI(TAG, "Initialisation complete. Scheduler running.");
}
