#include "log_ws.h"
#include "auth.h"
#include "sdkconfig.h"
#include "esp_log.h"
// #include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "LOG_WS";

#define MSG_MAX_LEN CONFIG_PILOT_LOG_WS_MSG_MAX_LEN
#define QUEUE_DEPTH CONFIG_PILOT_LOG_WS_QUEUE_DEPTH
#define MAX_WS_CLIENTS 4

/* ── Internal state ─────────────────────────────────────────────────────── */

typedef struct
{
  char data[MSG_MAX_LEN];
  uint16_t len;
} ws_msg_t;

static QueueHandle_t s_msg_queue;
static SemaphoreHandle_t s_clients_mutex;
static int s_client_fds[MAX_WS_CLIENTS];
static httpd_handle_t s_server_handle = NULL;

/* Original vprintf function pointer – we chain to it for UART output. */
static vprintf_like_t s_orig_vprintf = NULL;

/* ── Custom vprintf hook ────────────────────────────────────────────────── */

static int log_ws_vprintf(const char *fmt, va_list args)
{
  va_list args_copy;
  va_copy(args_copy, args);

  /* Forward to the original output (UART) first. */
  int ret = 0;
  if (s_orig_vprintf)
  {
    ret = s_orig_vprintf(fmt, args);
  }

  /* Build a JSON envelope and enqueue it (non-blocking). */
  char raw[MSG_MAX_LEN];
  vsnprintf(raw, sizeof(raw), fmt, args_copy);
  va_end(args_copy);

  /* Strip trailing newline for cleaner JSON. */
  size_t rlen = strlen(raw);
  if (rlen > 0 && raw[rlen - 1] == '\n')
    raw[--rlen] = '\0';

  ws_msg_t msg = {0};
  msg.len = (uint16_t)
      snprintf(msg.data, MSG_MAX_LEN, "{\"type\":\"log\",\"msg\":\"%s\"}", raw);
  /* Clamp to buffer. */
  if (msg.len >= MSG_MAX_LEN)
    msg.len = MSG_MAX_LEN - 1;

  /* Drop silently when the queue is full – logging must never block. */
  xQueueSendToBack(s_msg_queue, &msg, 0);

  return ret;
}

/* ── WebSocket broadcast task ───────────────────────────────────────────── */

static void log_ws_task(void *arg)
{
  // ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

  ESP_LOGI(TAG,
           "log_ws_task started (stack high-water: %u bytes)",
           uxTaskGetStackHighWaterMark(NULL));

  ws_msg_t msg;
  while (1)
  {
    // esp_task_wdt_reset();

    if (xQueueReceive(s_msg_queue, &msg, portMAX_DELAY) != pdTRUE)
      continue;
    if (!s_server_handle)
      continue;

    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
      if (s_client_fds[i] < 0)
        continue;

      httpd_ws_frame_t frame = {
          .type = HTTPD_WS_TYPE_TEXT,
          .payload = (uint8_t *) msg.data,
          .len = msg.len,
          .final = true,
      };
      esp_err_t err = httpd_ws_send_frame_async(s_server_handle,
                                                s_client_fds[i],
                                                &frame);
      if (err != ESP_OK)
      {
        /* Client likely disconnected; evict it. */
        ESP_LOGD(TAG,
                 "WS client fd=%d evicted (err=%s)",
                 s_client_fds[i],
                 esp_err_to_name(err));
        s_client_fds[i] = -1;
      }
    }
    xSemaphoreGive(s_clients_mutex);
  }
}

/* ── WebSocket HTTP handler ─────────────────────────────────────────────── */

static esp_err_t ws_upgrade_handler(httpd_req_t *req)
{
  if (req->method == HTTP_GET)
  {
    /* Upgrade handshake: authenticate via query-param token. */
    char query[128] = {0};
    char token[72] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
        && httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK)
    {
      if (!auth_validate_token(token))
      {
        ESP_LOGW(TAG, "WS upgrade rejected: invalid token");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Forbidden");
        return ESP_FAIL;
      }
    }
    else
    {
      ESP_LOGW(TAG, "WS upgrade rejected: no token");
      httpd_resp_set_status(req, "401 Unauthorized");
      httpd_resp_sendstr(req, "Forbidden");
      return ESP_FAIL;
    }

    /* Register the new client fd. */
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    bool added = false;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
      if (s_client_fds[i] < 0)
      {
        s_client_fds[i] = fd;
        added = true;
        break;
      }
    }
    xSemaphoreGive(s_clients_mutex);

    if (!added)
    {
      ESP_LOGW(TAG, "WS client table full, rejecting fd=%d", fd);
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_sendstr(req, "Too many clients");
      return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WS client connected: fd=%d", fd);
    return ESP_OK;
  }

  /* Receive frame (ping / close). */
  httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK)
    return err;

  if (frame.type == HTTPD_WS_TYPE_CLOSE || frame.type == HTTPD_WS_TYPE_PING)
  {
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
    {
      if (s_client_fds[i] == fd)
      {
        s_client_fds[i] = -1;
        break;
      }
    }
    xSemaphoreGive(s_clients_mutex);
    ESP_LOGI(TAG, "WS client disconnected: fd=%d", fd);
  }
  return ESP_OK;
}

static const httpd_uri_t s_ws_uri = {
    .uri = "/ws/log",
    .method = HTTP_GET,
    .handler = ws_upgrade_handler,
    .user_ctx = NULL,
    .is_websocket = true,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t log_ws_init(void)
{
  s_msg_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ws_msg_t));
  if (!s_msg_queue)
    return ESP_ERR_NO_MEM;

  s_clients_mutex = xSemaphoreCreateMutex();
  if (!s_clients_mutex)
    return ESP_ERR_NO_MEM;

  for (int i = 0; i < MAX_WS_CLIENTS; i++)
    s_client_fds[i] = -1;

  /* Chain custom vprintf – keep original for UART output. */
  s_orig_vprintf = esp_log_set_vprintf(log_ws_vprintf);

  BaseType_t rc = xTaskCreatePinnedToCore(log_ws_task,
                                          "log_ws",
                                          4096,
                                          NULL,
                                          5,
                                          NULL,
                                          1); // CPU1
  return (rc == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t log_ws_register(httpd_handle_t server)
{
  s_server_handle = server;
  return httpd_register_uri_handler(server, &s_ws_uri);
}

void log_ws_broadcast_json(const char *json_str)
{
  ws_msg_t msg = {0};
  msg.len = (uint16_t) snprintf(msg.data, MSG_MAX_LEN, "%s", json_str);
  if (msg.len >= MSG_MAX_LEN)
    msg.len = MSG_MAX_LEN - 1;
  xQueueSendToBack(s_msg_queue, &msg, 0);
}
