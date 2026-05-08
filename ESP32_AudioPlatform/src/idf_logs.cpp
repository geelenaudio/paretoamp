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

// Monotonic epoch: incremented on every new subscriber. The streaming
// task compares this against the epoch it was launched under on every
// loop iteration; if it drifts, the task exits so the new subscriber
// can take over. This gives "last-writer-wins" semantics — a stale
// browser tab keeping a dead socket open can't block a fresh reload
// from re-subscribing.
std::atomic<uint32_t> g_subscriber_epoch{0};

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

// Argument passed by handle_logs_stream to sse_stream_task. We can't stuff
// both the detached req and the epoch into the single `void*` task arg,
// so pack them in a small heap-allocated struct that the task `delete`s
// before exiting.
struct SseTaskArgs {
  httpd_req_t* req;
  uint32_t epoch;
};

// The streaming loop. Runs on its own task (see handle_logs_stream), NOT on
// the httpd worker, so a long-lived SSE connection can't starve other HTTP
// requests. `req` here is a *detached* copy produced by
// httpd_req_async_handler_begin(); we must hand it back via
// httpd_req_async_handler_complete() before returning so the httpd cleans
// up the socket slot.
void sse_stream_task(void* arg) {
  SseTaskArgs* args = (SseTaskArgs*)arg;
  httpd_req_t* req = args->req;
  const uint32_t my_epoch = args->epoch;
  delete args;

  char frame[kMaxLine + 80];
  char escaped[kMaxLine * 2];
  TickType_t heartbeat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);

  // Kick with a comment so the browser fires onopen immediately.
  if (!sse_send(req, ": connected\n\n", 13)) {
    goto done;
  }

  while (true) {
    // Bail fast if a newer subscriber took over. This is how "last-writer-
    // wins" is actually enforced: the new connection bumped the epoch, so
    // we spot it here and exit within at most one ring-receive window (~1 s).
    if (g_subscriber_epoch.load(std::memory_order_relaxed) != my_epoch) {
      break;
    }

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

    // No data in the last second — keep the connection alive with an SSE
    // comment. If the write fails, the peer disconnected and we exit.
    if ((int32_t)(xTaskGetTickCount() - heartbeat_deadline) >= 0) {
      if (!sse_send(req, ": ping\n\n", 8)) break;
      heartbeat_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    }
  }

done:
  // Only clear the "active" latch if we're still the current subscriber.
  // Otherwise the new task that just took over is the rightful owner and
  // has already set it to true for itself; we'd be clobbering it.
  if (g_subscriber_epoch.load(std::memory_order_relaxed) == my_epoch) {
    g_subscriber_active.store(false);
  }
  // Tell the httpd we're done with this detached request so the socket
  // can be freed and the async slot reused.
  httpd_req_async_handler_complete(req);
  vTaskDelete(nullptr);
}

esp_err_t handle_logs_stream(httpd_req_t* req) {
  // "Last-writer-wins": bump the epoch unconditionally. If a previous
  // streaming task is still alive, its next loop iteration will see the
  // drift and exit within ~1 s. Meanwhile we lay claim by setting the
  // active latch — fresh tasks spawned inside that 1 s overlap window
  // simply share the ring briefly (the old task stops reading once it
  // notices the epoch change), which is better than rejecting the client.
  const uint32_t my_epoch = g_subscriber_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  g_subscriber_active.store(true, std::memory_order_relaxed);

  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

  // Detach the socket from the httpd worker so the worker can return and
  // continue serving other requests. `detached` is a deep-copy of `req`
  // that stays valid across tasks; the worker's own `req` becomes invalid
  // as soon as we return ESP_OK here.
  httpd_req_t* detached = nullptr;
  esp_err_t rc = httpd_req_async_handler_begin(req, &detached);
  if (rc != ESP_OK || !detached) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "async begin failed");
  }

  // Heap-allocate the task args so both the req and epoch survive the
  // handoff through the single `void*` task parameter. The task `delete`s
  // it on entry.
  auto* args = new (std::nothrow) SseTaskArgs{detached, my_epoch};
  if (!args) {
    httpd_req_async_handler_complete(detached);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "sse args oom");
  }

  // Spawn a pinned task for the SSE loop. Core 1 mirrors the httpd worker
  // (see idf_http_server.cpp — moved off Core 0 because BT/Wi-Fi/lwip
  // saturate it during music playback). Lwip socket calls trampoline to
  // the TCPIP thread anyway, so colocating SSE with httpd on Core 1 keeps
  // both reachable while audio_task is blocked in i2s_channel_write.
  // Priority 4 is below audio (20) and httpd (10) so neither is starved
  // by a chatty log client; still above cpu-sampler (1) and idle (0).
  BaseType_t ok = xTaskCreatePinnedToCore(
      sse_stream_task, "sse-logs", 4096, args, /*prio*/ 4, nullptr,
      /*core*/ 1);
  if (ok != pdPASS) {
    delete args;
    httpd_req_async_handler_complete(detached);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "sse task spawn failed");
  }

  // Returning here unblocks the httpd worker *immediately*; the stream is
  // now owned by sse_stream_task.
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
