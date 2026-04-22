#include "log_ws.h"
#include "auth.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "LOG_WS";

#define MSG_MAX_LEN  CONFIG_PILOT_LOG_WS_MSG_MAX_LEN
#define QUEUE_DEPTH  CONFIG_PILOT_LOG_WS_QUEUE_DEPTH
#define MAX_WS_CLIENTS 4

typedef struct
{
    char     data[MSG_MAX_LEN];
    uint16_t len;
} ws_msg_t;

static QueueHandle_t     s_msg_queue     = NULL;
static SemaphoreHandle_t s_clients_mutex = NULL;
static int               s_client_fds[MAX_WS_CLIENTS];
static httpd_handle_t    s_server_handle = NULL;
static vprintf_like_t    s_orig_vprintf  = NULL;

/*
 * Re-entrancy guard for the vprintf hook.
 *
 * Prevents a stack-overflow scenario where any ESP_LOG* call issued from
 * within log_ws_vprintf itself (or from the httpd task while it processes a
 * work item queued by log_ws_task) re-enters this function.
 *
 * C11 atomic: lock-free on Xtensa LX7, safe to call from ISR context.
 */
static atomic_bool s_in_vprintf = ATOMIC_VAR_INIT(false);

/* ── ANSI CSI escape-sequence stripper ─────────────────────────────────── */

/**
 * @brief Copy @p src into @p dst, removing all ANSI CSI sequences and
 *        non-printable control characters (keeps \\t, \\n, \\r).
 *
 * State machine: ESC (0x1B) followed by '[' enters CSI state; any byte
 * in [0x40, 0x7E] terminates it.  Other ESC sequences (OSC, SS2, etc.)
 * are handled by consuming until the next printable byte.
 *
 * @param dst      Output buffer (NUL-terminated on return).
 * @param dst_max  Size of @p dst including the NUL terminator.
 * @param src      Input string (need not be NUL-terminated).
 * @param src_len  Number of bytes to process from @p src.
 * @return         Bytes written, excluding the NUL terminator.
 */
static size_t strip_ansi(char *dst, size_t dst_max,
                          const char *src, size_t src_len)
{
    typedef enum { ST_NORMAL, ST_ESC, ST_CSI } state_t;
    state_t state = ST_NORMAL;
    size_t  o     = 0;

    for (size_t i = 0; i < src_len && o < dst_max - 1U; i++) {
        unsigned char c = (unsigned char)src[i];

        switch (state) {
            case ST_NORMAL:
                if (c == 0x1BU) {                      /* ESC */
                    state = ST_ESC;
                } else if ((c >= 0x20U && c <= 0x7EU)  /* printable ASCII */
                           || c == '\t' || c == '\n' || c == '\r') {
                    dst[o++] = (char)c;
                }
                /* else: other control chars silently dropped */
                break;

            case ST_ESC:
                if (c == '[') {
                    state = ST_CSI;         /* CSI sequence start */
                } else if (c >= 0x40U && c <= 0x5FU) {
                    state = ST_NORMAL;      /* Two-byte ESC sequence done */
                } else {
                    state = ST_NORMAL;      /* Unknown; resync */
                }
                break;

            case ST_CSI:
                /* CSI parameter/intermediate bytes: 0x20-0x3F
                 * CSI final byte:                   0x40-0x7E  */
                if (c >= 0x40U && c <= 0x7EU) {
                    state = ST_NORMAL;      /* End of CSI sequence */
                }
                /* else: still consuming CSI parameters, keep looping */
                break;
        }
    }

    dst[o] = '\0';
    return o;
}

/* ── Custom vprintf hook (zero heap allocation) ─────────────────────────── */

static int log_ws_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    /* Always forward to the original handler first (UART output). */
    int ret = 0;
    if (s_orig_vprintf) {
        ret = s_orig_vprintf(fmt, args);
    }

    /* Guard: skip if queue not ready or we are already inside this hook.   */
    if (!s_msg_queue) {
        va_end(args_copy);
        return ret;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_in_vprintf, &expected, true)) {
        /* Re-entrant call – drop to prevent stack overflow. */
        va_end(args_copy);
        return ret;
    }

    /* ── Format the raw log line ── */
    char raw[MSG_MAX_LEN / 2];
    int  vlen = vsnprintf(raw, sizeof(raw), fmt, args_copy);
    va_end(args_copy);

    if (vlen <= 0) {
        atomic_store(&s_in_vprintf, false);
        return ret;
    }

    size_t rlen = ((size_t)vlen >= sizeof(raw)) ? sizeof(raw) - 1U
                                                : (size_t)vlen;

    /* Strip trailing CR/LF. */
    while (rlen > 0U && (raw[rlen - 1U] == '\n' || raw[rlen - 1U] == '\r')) {
        raw[--rlen] = '\0';
    }

    /* ── Strip ANSI escape sequences ── */
    char clean[MSG_MAX_LEN / 2];
    size_t clen = strip_ansi(clean, sizeof(clean), raw, rlen);

    if (clen == 0U) {
        atomic_store(&s_in_vprintf, false);
        return ret;
    }

    /* ── Build JSON frame directly in ws_msg_t (zero heap) ── */
    ws_msg_t msg = {0};
    size_t   idx = 0U;

    static const char k_prefix[] = "{\"type\":\"log\",\"msg\":\"";
    for (const char *p = k_prefix; *p && idx < MSG_MAX_LEN - 1U; ) {
        msg.data[idx++] = *p++;
    }

    for (size_t i = 0U; i < clen && idx < MSG_MAX_LEN - 4U; i++) {
        char c = clean[i];
        switch (c) {
            case '"':  msg.data[idx++] = '\\'; msg.data[idx++] = '"';  break;
            case '\\': msg.data[idx++] = '\\'; msg.data[idx++] = '\\'; break;
            case '\n': msg.data[idx++] = '\\'; msg.data[idx++] = 'n';  break;
            case '\r': msg.data[idx++] = '\\'; msg.data[idx++] = 'r';  break;
            case '\t': msg.data[idx++] = '\\'; msg.data[idx++] = 't';  break;
            default:
                if (c >= 0x20 && c <= 0x7E) msg.data[idx++] = c;
                break;
        }
    }

    static const char k_suffix[] = "\"}";
    for (const char *p = k_suffix; *p && idx < MSG_MAX_LEN - 1U; ) {
        msg.data[idx++] = *p++;
    }

    msg.data[idx] = '\0';
    msg.len = (uint16_t)idx;

    atomic_store(&s_in_vprintf, false);

    /* Enqueue: non-blocking – messages are dropped if the queue is full.
     * This is intentional: the hook must never block the calling task.     */
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xQueueSendToBackFromISR(s_msg_queue, &msg, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xQueueSendToBack(s_msg_queue, &msg, 0);
    }

    return ret;
}

/* ── Client table helpers ───────────────────────────────────────────────── */

static void client_add(int fd)
{
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_client_fds[i] < 0) {
            s_client_fds[i] = fd;
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

static void client_remove(int fd)
{
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_client_fds[i] == fd) {
            s_client_fds[i] = -1;
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

/* ── WebSocket broadcast task ───────────────────────────────────────────── */

static void log_ws_task(void *arg)
{
    ws_msg_t msg;
    int      local_fds[MAX_WS_CLIENTS];

    while (1) {
        if (xQueueReceive(s_msg_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_server_handle || msg.len == 0U) {
            continue;
        }

        /* Snapshot the fd table under the mutex, then release immediately.
         * The actual send must NOT hold the mutex – it may block internally. */
        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        memcpy(local_fds, s_client_fds, sizeof(local_fds));
        xSemaphoreGive(s_clients_mutex);

        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (local_fds[i] < 0) continue;

            httpd_ws_frame_t frame = {
                .type       = HTTPD_WS_TYPE_TEXT,
                .payload    = (uint8_t *)msg.data,
                .len        = msg.len,
                .fragmented = false,
                .final      = true,
            };

            esp_err_t err = httpd_ws_send_frame_async(s_server_handle,
                                                      local_fds[i],
                                                      &frame);

            if (err != ESP_OK)
            {
              switch (err)
              {
              case ESP_ERR_INVALID_ARG: /* bad fd or NULL args     */
              case ESP_ERR_NOT_FOUND:   /* fd not known to httpd   */
              case ESP_ERR_NO_MEM:      /* heap exhausted          */
                /* Remove the stale descriptor from the table. */
                xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
                if (s_client_fds[i] == local_fds[i])
                {
                  s_client_fds[i] = -1;
                }
                xSemaphoreGive(s_clients_mutex);
                ESP_LOGD(TAG,
                         "Removed stale WS fd=%d (err=%s)",
                         local_fds[i],
                         esp_err_to_name(err));
                break;

              case ESP_ERR_INVALID_STATE:
                /* * Race condition guard: During the HTTP GET handshake, the 
                     * HTTPD core has not yet set ws_handshake_done = true. 
                     * Sending a frame now yields ESP_ERR_INVALID_STATE.
                     * Do not remove the fd; it will be valid shortly.
                     */
                ESP_LOGD(TAG, "WS handshake pending for fd=%d", local_fds[i]);
                break;

              case ESP_FAIL:
                /* httpd internal work-queue full – keep the client,
                     * this message is lost but subsequent ones may succeed.
                     */
                ESP_LOGD(TAG,
                         "httpd work queue full, message dropped for fd=%d",
                         local_fds[i]);
                break;

              default:
                break;
              }
            }
        }
    }
}

/* ── WebSocket HTTP handler ─────────────────────────────────────────────── */

static esp_err_t ws_upgrade_handler(httpd_req_t *req)
{
    /* ── Initial HTTP GET: perform WebSocket upgrade ── */
    if (req->method == HTTP_GET) {
        char query[128] = {0};
        char token[72]  = {0};

        bool token_ok =
            (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) &&
            (httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK) &&
            auth_validate_token(token);

        if (!token_ok) {
            ESP_LOGW(TAG, "WS upgrade rejected: invalid or missing token");
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_sendstr(req, "Forbidden");
            return ESP_FAIL;
        }

        int fd = httpd_req_to_sockfd(req);
        bool added = false;

        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_client_fds[i] < 0) {
                s_client_fds[i] = fd;
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_clients_mutex);

        if (!added) {
            ESP_LOGW(TAG, "WS client table full, rejecting fd=%d", fd);
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "Too many clients");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "WS client connected: fd=%d", fd);
        return ESP_OK;
    }

    /* ── Subsequent call: incoming WebSocket frame from client ── */

    /* Peek the frame header only (max_len=0) to determine type and length.  */
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    int fd = httpd_req_to_sockfd(req);

    switch (frame.type) {
        case HTTPD_WS_TYPE_CLOSE:
            /*
             * Drain the optional 2-byte status code from the socket buffer.
             * Failing to read it causes subsequent httpd reads on this socket
             * to see stale bytes, corrupting the framing parser.
             */
            if (frame.len > 0U) {
                uint8_t close_payload[4] = {0};
                frame.payload = close_payload;
                httpd_ws_recv_frame(req, &frame,
                                    frame.len < sizeof(close_payload)
                                        ? frame.len : sizeof(close_payload));
            }
            client_remove(fd);
            ESP_LOGI(TAG, "WS CLOSE received: fd=%d", fd);
            break;

        case HTTPD_WS_TYPE_PING:
            /*
             * RFC 6455 §5.5.2: a PONG must be sent in response to every PING.
             * Silently dropping or disconnecting the client on PING is a
             * protocol violation that causes compliant browsers to close
             * the connection.
             */
            {
                httpd_ws_frame_t pong = {
                    .type       = HTTPD_WS_TYPE_PONG,
                    .len        = 0U,
                    .fragmented = false,
                    .final      = true,
                };
                httpd_ws_send_frame(req, &pong);
                ESP_LOGD(TAG, "WS PING→PONG: fd=%d", fd);
            }
            break;

        case HTTPD_WS_TYPE_PONG:
        case HTTPD_WS_TYPE_TEXT:
        case HTTPD_WS_TYPE_BINARY:
        default:
            /*
             * This is a server-to-client log stream; clients should not send
             * data back. Drain any payload to keep the socket buffer clean.
             */
            if (frame.len > 0U) {
                uint8_t *drain = malloc(frame.len);
                if (drain) {
                    frame.payload = drain;
                    httpd_ws_recv_frame(req, &frame, frame.len);
                    free(drain);
                }
            }
            break;
    }

    return ESP_OK;
}

/*
 * FIX: URI must be "/ws/log" (exact path, no wildcard suffix).
 *
 * The default esp_http_server URI matcher strips the query string before
 * comparing, so "/ws/log" correctly matches "/ws/log?token=<value>".
 *
 * The previous "/ws/log*" caused a length mismatch:
 *   template length = 8  ("/ w s / l o g *")
 *   request path    = 7  ("/ w s / l o g")   → NO MATCH
 *
 * If the application requires wildcard matching for other handlers, set
 *   config.uri_match_fn = httpd_uri_match_wildcard
 * in web_server.c AND use "/ws/log*" here – but NOT a mix of the two.
 */
static const httpd_uri_t s_ws_uri = {
    .uri          = "/ws/log",
    .method       = HTTP_GET,
    .handler      = ws_upgrade_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t log_ws_init(void)
{
    s_msg_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ws_msg_t));
    if (!s_msg_queue) return ESP_ERR_NO_MEM;

    s_clients_mutex = xSemaphoreCreateMutex();
    if (!s_clients_mutex) return ESP_ERR_NO_MEM;

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_client_fds[i] = -1;
    }

    s_orig_vprintf = esp_log_set_vprintf(log_ws_vprintf);

    BaseType_t rc = xTaskCreate(
        log_ws_task, "log_ws", 4096, NULL, 3, NULL);

    return (rc == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t log_ws_register(httpd_handle_t server)
{
    s_server_handle = server;
    return httpd_register_uri_handler(server, &s_ws_uri);
}

void log_ws_on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    /*
     * Called by esp_http_server when ANY socket is closed (normal or error).
     * Ensures zombie fds never accumulate in s_client_fds[].
     *
     * Wiring required in web_server.c:
     *   httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
     *   cfg.httpd.close_fn = log_ws_on_close;
     */
    client_remove(fd);
    ESP_LOGD(TAG, "Socket closed (close_fn): fd=%d", fd);
}

void log_ws_broadcast_json(const char *json_str)
{
    if (!json_str || !s_msg_queue) return;

    ws_msg_t msg = {0};
    int n = snprintf(msg.data, MSG_MAX_LEN, "%s", json_str);
    msg.len = (n > 0 && (size_t)n < MSG_MAX_LEN)
                  ? (uint16_t)n
                  : (uint16_t)(MSG_MAX_LEN - 1U);

    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xQueueSendToBackFromISR(s_msg_queue, &msg, &woken);
        portYIELD_FROM_ISR(woken);
    } else {
        xQueueSendToBack(s_msg_queue, &msg, 0);
    }
}
