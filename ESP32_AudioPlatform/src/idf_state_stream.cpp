// See idf_state_stream.h for the design.

#include "idf_state_stream.h"
#include "idf_api.h"
#include "idf_config_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "state-stream";

// Caps come from `httpd.max_open_sockets = 10`. We reserve a few for the
// log SSE + regular GETs, so 6 concurrent state subscribers is the most
// any reasonable SPA fleet would hit (1 user × 4 tabs is the realistic
// upper bound). New connections beyond this are 503'd with a Retry-After
// hint rather than booting the oldest, so opening a fifth tab never
// disconnects the visible four.
constexpr size_t kMaxSubscribers = 6;

// Broadcaster cadence. 250 ms is the smallest interval that still feels
// "instant" to a UI without amplifying slider-drag bursts; ConfigManager
// itself debounces NVS writes on a similar timer, so no slider produces
// more frames than this regardless of how fast the user moves.
constexpr int kBroadcastPeriodMs = 250;

// Snapshot framing buffer. Sized for the largest realistic state JSON
// (full EQ curve + test-tone settings + identity). 4 KiB is comfortable.
constexpr size_t kFrameCapBytes = 4096;

struct Subscriber {
  httpd_req_t* req;     // detached request
  uint32_t epoch;       // assigned at connect; broadcaster checks for staleness
  uint32_t last_sent_v; // last stateVersion the broadcaster pushed to this client
  bool alive;
};

ConfigManager* s_cfg = nullptr;
SemaphoreHandle_t s_lock = nullptr;
Subscriber s_subs[kMaxSubscribers]{};
size_t s_sub_count = 0;

// Broadcaster task is created lazily on the first subscriber. Keeping
// it dormant during boot is a deliberate DRAM-management choice:
// xTaskCreatePinnedToCore allocates the 4 KiB stack from
// `MALLOC_CAP_INTERNAL`, the same pool Bluedroid pulls its host-side
// queues from. Spawning it eagerly in `stateStreamInit` (called before
// `g_a2dp.begin`) shaves enough off the contiguous-block budget to push
// `gatt_init`'s `fixed_queue_new` over the cliff and bootloop the
// device with a NULL-queue assert. See `idf_a2dp_sink.cpp` for the
// matching heap log line.
std::atomic<bool> s_broadcaster_started{false};

// Force-publish flag; bumped by stateStreamMarkDirty for callers whose
// mutation didn't go through ConfigManager::markDirty*.
std::atomic<bool> s_force_publish{false};

// Last version we serialized. Compared each tick against cfg->stateVersion()
// to decide whether to rebuild the snapshot.
std::atomic<uint32_t> s_last_published_v{0};

// Locking helpers.
struct Lock {
  Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_lock); }
};

bool sse_send(httpd_req_t* req, const char* data, size_t n) {
  return httpd_resp_send_chunk(req, data, n) == ESP_OK;
}

void close_and_drop_unlocked(size_t i) {
  if (!s_subs[i].alive) return;
  s_subs[i].alive = false;
  if (s_subs[i].req) {
    // `_complete` is what releases the httpd's session slot. Without it, a
    // dropped client would block one of our 10 sockets until LRU purge.
    httpd_req_async_handler_complete(s_subs[i].req);
    s_subs[i].req = nullptr;
  }
}

// Compact subscriber array after drops so iteration stays cheap. Caller holds the lock.
void compact_unlocked() {
  size_t w = 0;
  for (size_t r = 0; r < s_sub_count; ++r) {
    if (s_subs[r].alive) {
      if (w != r) s_subs[w] = s_subs[r];
      ++w;
    }
  }
  s_sub_count = w;
}

// Build "data: …\n\n" framed SSE event with explicit type. Returns bytes
// written to `frame` (may be 0 if json_text doesn't fit in the cap minus
// framing overhead).
size_t format_frame(char* frame, size_t cap, const char* event_name,
                    uint32_t version, const char* json_text) {
  // event: <name>\nid: <v>\ndata: <json>\n\n
  // Keep the data on a single line — RFC 6202-style multi-line `data:` is
  // legal but the SPA we ship just concatenates whatever it sees, and
  // cJSON_PrintUnformatted produces no embedded newlines.
  int n = snprintf(frame, cap, "event: %s\nid: %u\ndata: %s\n\n",
                   event_name, (unsigned)version, json_text);
  if (n <= 0 || (size_t)n >= cap) return 0;
  return (size_t)n;
}

// Serialize the current state and broadcast the snapshot. Drops any
// subscriber whose chunk write fails. Caller need NOT hold the lock —
// this routine takes it itself.
void publish_snapshot() {
  if (!s_cfg) return;
  cJSON* root = buildStateJson(s_cfg);
  if (!root) return;
  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) return;

  const uint32_t version = s_cfg->stateVersion();
  s_last_published_v.store(version, std::memory_order_release);

  // One frame buffer reused across subscribers. A larger snapshot (e.g.
  // post-Phase-3 with mode-state metadata) would exceed kFrameCapBytes;
  // we log + skip rather than truncate so subscribers don't see an
  // incomplete JSON document.
  static char frame[kFrameCapBytes];
  size_t n = format_frame(frame, sizeof(frame), "snapshot", version, json);
  free(json);
  if (n == 0) {
    ESP_LOGE(TAG, "snapshot too large for frame buffer (cap=%u); skip", (unsigned)kFrameCapBytes);
    return;
  }

  Lock lk;
  bool any_dropped = false;
  for (size_t i = 0; i < s_sub_count; ++i) {
    if (!s_subs[i].alive) continue;
    if (!sse_send(s_subs[i].req, frame, n)) {
      ESP_LOGI(TAG, "subscriber %u dropped (send failed)", (unsigned)i);
      close_and_drop_unlocked(i);
      any_dropped = true;
      continue;
    }
    s_subs[i].last_sent_v = version;
  }
  if (any_dropped) compact_unlocked();
}

// Send a heartbeat comment so middleboxes don't time out the TCP. SSE
// "event: ping" with empty data is the conventional shape; some browsers
// surface it as a named event but never fire onmessage, which is exactly
// what we want.
void publish_heartbeat() {
  static const char kPing[] = "event: ping\ndata: 0\n\n";
  Lock lk;
  bool any_dropped = false;
  for (size_t i = 0; i < s_sub_count; ++i) {
    if (!s_subs[i].alive) continue;
    if (!sse_send(s_subs[i].req, kPing, sizeof(kPing) - 1)) {
      close_and_drop_unlocked(i);
      any_dropped = true;
    }
  }
  if (any_dropped) compact_unlocked();
}

void broadcaster_task(void*);

// Spawn the broadcaster task on demand. Called under `s_lock` from
// `handle_state_stream` when the first subscriber is about to be
// registered. Idempotent — subsequent calls are no-ops.
void ensure_broadcaster_started_locked() {
  bool expected = false;
  if (!s_broadcaster_started.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) {
    return;  // already running
  }
  // 4 KiB stack on Core 1 prio 4 — same neighbourhood as `sse-logs`,
  // well below httpd (10) and audio_task (20). Stack is allocated in
  // PSRAM (`MALLOC_CAP_SPIRAM`) so the broadcaster does not compete
  // with Bluedroid's L2CAP/SDP CCB allocations for the limited
  // internal-DRAM pool when a phone connects mid-stream. cJSON itself
  // uses heap (8-bit cap by default), not stack, so the slower PSRAM
  // access doesn't affect snapshot throughput. Safe: broadcaster does
  // no FPU work and runs from task context only.
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      broadcaster_task, "state-bc", /*stack*/ 4096, nullptr, /*prio*/ 4,
      nullptr, /*core*/ 1, MALLOC_CAP_SPIRAM);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "broadcaster task create failed");
    s_broadcaster_started.store(false, std::memory_order_release);
  }
}

void broadcaster_task(void*) {
  ESP_LOGI(TAG, "state stream broadcaster up (period=%d ms, max_subs=%u)",
           kBroadcastPeriodMs, (unsigned)kMaxSubscribers);
  TickType_t last_heartbeat = xTaskGetTickCount();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kBroadcastPeriodMs));

    // Cheap: a single uint32 read in ConfigManager (a relaxed-atomic
    // counter is fine for this comparison; we never lock cfg here).
    const uint32_t cur_v = s_cfg ? s_cfg->stateVersion() : 0;
    const uint32_t last_v = s_last_published_v.load(std::memory_order_acquire);
    const bool forced = s_force_publish.exchange(false, std::memory_order_acq_rel);
    const bool changed = (cur_v != last_v);

    if ((changed || forced) && s_sub_count > 0) {
      publish_snapshot();
    } else if (s_sub_count > 0) {
      // 10 s heartbeat — long enough to be invisible to the SPA's RTT but
      // short enough that load balancers / reverse proxies don't half-close
      // an idle SSE socket.
      if ((xTaskGetTickCount() - last_heartbeat) >= pdMS_TO_TICKS(10000)) {
        publish_heartbeat();
        last_heartbeat = xTaskGetTickCount();
      }
    } else {
      last_heartbeat = xTaskGetTickCount();
    }
  }
}

esp_err_t handle_state_stream(httpd_req_t* req) {
  if (!s_cfg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                         "state-stream not initialized");

  // Capacity check upfront so we can answer with a clean 503 instead of
  // accepting and immediately dropping (which a browser would just retry
  // forever, defeating the point). esp_http_server's `HTTPD_xxx_*` enum
  // does not include 503, so we set the status line + body manually —
  // the SPA's EventSource auto-reconnect will respect the Retry-After.
  {
    Lock lk;
    if (s_sub_count >= kMaxSubscribers) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_set_hdr(req, "Retry-After", "5");
      return httpd_resp_send(req, "state stream subscriber limit reached", HTTPD_RESP_USE_STRLEN);
    }
  }

  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

  // Seed the connection with the current snapshot before detaching, so the
  // SPA can render immediately without a follow-up REST round-trip. If
  // this initial send fails, the client will simply retry — no async slot
  // leaks because we haven't called async_handler_begin yet.
  cJSON* root = buildStateJson(s_cfg);
  char* initial_json = root ? cJSON_PrintUnformatted(root) : nullptr;
  if (root) cJSON_Delete(root);
  if (!initial_json) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot oom");
  }
  const uint32_t initial_v = s_cfg->stateVersion();
  char initial_frame[kFrameCapBytes];
  size_t n = format_frame(initial_frame, sizeof(initial_frame), "snapshot",
                          initial_v, initial_json);
  free(initial_json);
  if (n == 0 || httpd_resp_send_chunk(req, initial_frame, n) != ESP_OK) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot send failed");
  }

  // Detach so the httpd worker can return; broadcaster_task will use the
  // detached `req` for all subsequent chunks.
  httpd_req_t* detached = nullptr;
  if (httpd_req_async_handler_begin(req, &detached) != ESP_OK || !detached) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
  }

  Lock lk;
  // Race-recheck under the lock — another connection might have used the
  // last slot between our initial check and now. Unlikely in practice
  // (single broadcaster task) but cheap to be safe.
  if (s_sub_count >= kMaxSubscribers) {
    httpd_req_async_handler_complete(detached);
    // We've already started the SSE response (initial snapshot was
    // chunk-sent above), so we cannot rewrite the status line at this
    // point. Just close the chunked stream cleanly with an empty
    // terminator and let the broadcaster never see this client.
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
  }

  static std::atomic<uint32_t> s_epoch_seq{1};
  s_subs[s_sub_count] = Subscriber{
      detached,
      s_epoch_seq.fetch_add(1, std::memory_order_relaxed),
      initial_v,
      true,
  };
  ++s_sub_count;
  ESP_LOGI(TAG, "subscriber added (count=%u)", (unsigned)s_sub_count);
  // First subscriber: bring the broadcaster online. `stateStreamInit`
  // intentionally does *not* spawn the task at boot — its 4 KiB stack
  // is allocated from the same `MALLOC_CAP_INTERNAL` pool that
  // Bluedroid's host-side queues need at `g_a2dp.begin` time, and on
  // this build the contiguous-block budget is too tight to spend it
  // before BT is up. Idempotent on subsequent subscribers.
  ensure_broadcaster_started_locked();
  return ESP_OK;
}

}  // namespace

void stateStreamInit(ConfigManager* cfg) {
  if (s_cfg) return;
  s_cfg = cfg;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    ESP_LOGE(TAG, "mutex create failed");
    s_cfg = nullptr;
    return;
  }
  // The broadcaster task is *not* spawned here — it's lazily created on
  // the first SSE subscriber via `ensure_broadcaster_started_locked`.
  // Rationale: at `app_main` time the internal-DRAM pool is tight and
  // Bluedroid's host-side init (`g_a2dp.begin`) needs the contiguous
  // headroom. With no UI tabs open the broadcaster has no work anyway,
  // so deferring the 4 KiB stack costs nothing in practice.
  ESP_LOGI(TAG, "state stream initialised (broadcaster spawns on first subscriber)");
}

void stateStreamRegister(httpd_handle_t server) {
  static const httpd_uri_t route = {
      .uri = "/api/state/stream",
      .method = HTTP_GET,
      .handler = handle_state_stream,
      .user_ctx = nullptr,
  };
  esp_err_t e = httpd_register_uri_handler(server, &route);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "register /api/state/stream failed: %s", esp_err_to_name(e));
  }
}

void stateStreamMarkDirty() {
  s_force_publish.store(true, std::memory_order_release);
}

}  // namespace geelen_idf
