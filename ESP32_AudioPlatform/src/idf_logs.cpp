// Live log ring buffer + SSE stream. See idf_logs.h for the design.
#include "idf_logs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace geelen_idf {

namespace {

// Size chosen so a burst of a few hundred lines survives the interval
// between client poll and SSE reconnect. 16 KiB of PSRAM-or-DRAM keeps
// roughly 120–200 typical log lines.
constexpr size_t kRingBytes = 16 * 1024;

// Per-line cap. Matches ESP-IDF's internal logger line cap.
constexpr size_t kMaxLine = 400;

RingbufHandle_t g_ring = nullptr;
vprintf_like_t  g_prev_vprintf = nullptr;

// Single-subscriber latch. false = nobody streaming, so we can skip the
// ring write altogether? No — we still want to capture for the next
// connect. So this is purely informational.
std::atomic<bool> g_subscriber_active{false};

// Hook invoked by ESP_LOGx. Runs in whatever task emitted the log.
int log_vprintf_hook(const char* fmt, va_list ap) {
  // va_list is single-use, copy once for the side channel.
  va_list ap_copy;
  va_copy(ap_copy, ap);

  int uart_ret = 0;
  if (g_prev_vprintf) {
    uart_ret = g_prev_vprintf(fmt, ap);
  } else {
    uart_ret = vprintf(fmt, ap);
  }

  if (g_ring) {
    char line[kMaxLine];
    int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
    if (n > 0) {
      size_t len = (n >= (int)sizeof(line)) ? sizeof(line) - 1 : (size_t)n;
      // Strip trailing newline; the SSE framing adds its own.
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        --len;
      }
      if (len > 0) {
        // Non-blocking send; if full, drop oldest until we fit. The ring
        // does not auto-evict, so we loop a small number of receives.
        for (int attempt = 0; attempt < 8; ++attempt) {
          if (xRingbufferSend(g_ring, line, len, 0) == pdTRUE) break;
          size_t victim_size = 0;
          void* victim = xRingbufferReceive(g_ring, &victim_size, 0);
          if (!victim) break;
          vRingbufferReturnItem(g_ring, victim);
        }
      }
    }
  }

  va_end(ap_copy);
  return uart_ret;
}

// Write a pre-formatted SSE frame. Returns false if the socket is gone,
// which is our signal to exit the stream loop.
bool sse_send(httpd_req_t* req, const char* data, size_t n) {
  return httpd_resp_send_chunk(req, data, n) == ESP_OK;
}

// Escape a raw log line into a JSON string body (no surrounding quotes).
// Very small subset: \ " \n \r \t plus control chars as \uXXXX.
size_t json_escape(const char* src, size_t n, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 8 < cap; ++i) {
    unsigned char c = (unsigned char)src[i];
    switch (c) {
      case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
      case '"':  out[o++] = '\\'; out[o++] = '"';  break;
      case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
      case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
      case '\t': out[o++] = '\\'; out[o++] = 't';  break;
      default:
        if (c < 0x20) {
          o += snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
          out[o++] = (char)c;
        }
    }
  }
  out[o] = '\0';
  return o;
}

esp_err_t handle_logs_stream(httpd_req_t* req) {
  if (g_subscriber_active.exchange(true)) {
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "another subscriber already attached");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

  // Kick with a comment so the browser opens the stream immediately.
  sse_send(req, ": connected\n\n", 13);

  char frame[kMaxLine + 80];
  char escaped[kMaxLine * 2];
  TickType_t heartbeat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);

  while (true) {
    size_t item_size = 0;
    char* item = (char*)xRingbufferReceive(
        g_ring, &item_size, pdMS_TO_TICKS(1000));

    if (item) {
      size_t n = json_escape(item, item_size, escaped, sizeof(escaped));
      vRingbufferReturnItem(g_ring, item);

      uint64_t t_ms = esp_timer_get_time() / 1000ULL;
      int fn = snprintf(frame, sizeof(frame),
                        "data: {\"t\":%llu,\"line\":\"%.*s\"}\n\n",
                        (unsigned long long)t_ms, (int)n, escaped);
      if (fn <= 0 || !sse_send(req, frame, (size_t)fn)) break;
      heartbeat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
      continue;
    }

    // No data — keep the connection alive with an SSE comment.
    if ((int32_t)(xTaskGetTickCount() - heartbeat_deadline) >= 0) {
      if (!sse_send(req, ": ping\n\n", 8)) break;
      heartbeat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    }
  }

  g_subscriber_active.store(false);
  return ESP_OK;
}

}  // namespace

void logsInit() {
  if (g_ring) return;
  g_ring = xRingbufferCreate(kRingBytes, RINGBUF_TYPE_NOSPLIT);
  if (!g_ring) return;
  g_prev_vprintf = esp_log_set_vprintf(&log_vprintf_hook);
}

void logsRegisterStream(httpd_handle_t server) {
  static const httpd_uri_t route = {
      .uri = "/api/logs/stream",
      .method = HTTP_GET,
      .handler = handle_logs_stream,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(server, &route);
}

}  // namespace geelen_idf
