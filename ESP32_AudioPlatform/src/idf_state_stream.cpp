// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// See idf_state_stream.h for the design.

#include "idf_state_stream.h"
#include "idf_api.h"
#include "idf_config_manager.h"
#include "idf_a2dp_sink.h"
#include "idf_voice_feedback.h"

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

#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "state-stream";

// Caps come from `httpd.max_open_sockets = 10` *minus* the working set the
// device cannot live without while subscribers are active:
//   - 1 SSE log slot (idf_logs.cpp)
//   - 2 async file-send slots (idf_http_server.cpp serve_file path)
//   - 2-3 transient REST handlers (state polls, EQ writes, scan)
//   - LRU + httpd internal slack
// Each SSE state subscriber pins ~3 lwip TX pbufs (~4.5 KiB internal DRAM)
// in steady state, plus the detached httpd_req_t (~600 B). We measured
// the device falling into a `wifi:m f null`/`BT_APPL: Pkt dropped` storm
// at 4-5 simultaneous subscribers with A2DP playing — the contiguous
// internal-DRAM pool drops to <4 KiB and lwip can't allocate fresh pbufs
// for inbound Wi-Fi frames. 3 is the highest cap that survives.
//
// This is also the cap that protects against EventSource auto-reconnect
// storms: when the device transiently fails a send, the browser opens a
// fresh SSE while the OLD one is still in `s_subs` (we only detect dead
// sockets on the next broadcast). The smaller cap + the pre-accept prune
// (`prune_dead_subscribers_unlocked`) below stop the ghost pile-up.
constexpr size_t kMaxSubscribers = 3;

// Broadcaster cadence. 250 ms is the smallest interval that still feels
// "instant" to a UI without amplifying slider-drag bursts; ConfigManager
// itself debounces NVS writes on a similar timer, so no slider produces
// more frames than this regardless of how fast the user moves.
constexpr int kBroadcastPeriodMs = 250;
constexpr int kBroadcastPeriodStreamingMs = 500;

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

// Previous full-state tree, used to compute `event: diff` frames.
cJSON* s_last_state = nullptr;

// If more than this many top-level keys changed, send a full snapshot
// instead of a diff (the EQ curve alone can dominate the payload).
constexpr int kMaxDiffKeys = 4;

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

// Probe each subscriber's underlying socket and drop any whose peer has
// closed the connection (FIN observed) or errored out. Caller holds the
// lock. Designed to be called *before* registering a new subscriber so
// EventSource auto-reconnect doesn't pile up zombie slots — the typical
// failure mode is: (1) old socket sees a transient lwip ENOMEM, (2) IDF
// httpd quietly half-closes it, (3) browser silently opens a fresh SSE,
// (4) the old s_subs slot stays "alive" until the next broadcast send
// fails on it (up to 250 ms + however long lwip takes to give us EAGAIN).
// During that window the count grows unbounded and pbufs pile up, which
// is what triggered the wifi:m f null / Pkt dropped cascade in the
// previous bug report.
//
// Detection uses `recv(MSG_PEEK | MSG_DONTWAIT)`:
//   r == 0  → peer sent FIN (clean close)        → drop
//   r > 0   → unexpected data on an SSE socket   → drop (broken client)
//   r < 0 + errno == EAGAIN/EWOULDBLOCK → alive  → keep
//   r < 0 + other errno → socket error           → drop
size_t prune_dead_subscribers_unlocked() {
  size_t pruned = 0;
  for (size_t i = 0; i < s_sub_count; ++i) {
    if (!s_subs[i].alive) continue;
    int fd = httpd_req_to_sockfd(s_subs[i].req);
    if (fd < 0) {
      close_and_drop_unlocked(i);
      ++pruned;
      continue;
    }
    char probe;
    ssize_t r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r > 0 || r == 0 ||
        (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      ESP_LOGI(TAG, "subscriber %u pruned (probe: r=%d errno=%d)",
               (unsigned)i, (int)r, (int)errno);
      close_and_drop_unlocked(i);
      ++pruned;
    }
  }
  if (pruned) compact_unlocked();
  return pruned;
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

cJSON* build_top_level_diff(cJSON* prev, cJSON* cur) {
  if (!prev || !cur) return nullptr;
  cJSON* diff = cJSON_CreateObject();
  if (!diff) return nullptr;
  int nkeys = 0;
  for (cJSON* c = cur->child; c; c = c->next) {
    cJSON* p = cJSON_GetObjectItemCaseSensitive(prev, c->string);
    char* cs = cJSON_PrintUnformatted(c);
    char* ps = p ? cJSON_PrintUnformatted(p) : nullptr;
    const bool changed = !cs || !ps || std::strcmp(cs, ps) != 0;
    free(cs);
    free(ps);
    if (!changed) continue;
    cJSON_AddItemToObject(diff, c->string, cJSON_Duplicate(c, 1));
    ++nkeys;
  }
  for (cJSON* p = prev->child; p; p = p->next) {
    if (cJSON_GetObjectItemCaseSensitive(cur, p->string)) continue;
    cJSON_AddNullToObject(diff, p->string);
    ++nkeys;
  }
  if (nkeys == 0 || nkeys > kMaxDiffKeys) {
    cJSON_Delete(diff);
    return nullptr;
  }
  return diff;
}

bool send_frame_to_subs(const char* frame, size_t n, uint32_t version,
                        bool snapshot_only_new) {
  Lock lk;
  bool any_dropped = false;
  for (size_t i = 0; i < s_sub_count; ++i) {
    if (!s_subs[i].alive) continue;
    if (snapshot_only_new && s_subs[i].last_sent_v != 0) continue;
    // Diff frames skip anyone who already received this version (a full
    // snapshot sent earlier in the same publish).
    if (!snapshot_only_new && s_subs[i].last_sent_v == version) continue;
    if (!sse_send(s_subs[i].req, frame, n)) {
      ESP_LOGI(TAG, "subscriber %u dropped (send failed)", (unsigned)i);
      close_and_drop_unlocked(i);
      any_dropped = true;
      continue;
    }
    s_subs[i].last_sent_v = version;
  }
  if (any_dropped) compact_unlocked();
  return true;
}

// Serialize the current state and broadcast a snapshot (new subscribers)
// and/or a compact top-level diff (existing subscribers).
void publish_snapshot() {
  if (!s_cfg) return;
  cJSON* root = buildStateJson(s_cfg);
  if (!root) return;

  const uint32_t version = s_cfg->stateVersion();
  s_last_published_v.store(version, std::memory_order_release);

  char* snap_json = cJSON_PrintUnformatted(root);
  if (!snap_json) {
    cJSON_Delete(root);
    return;
  }

  cJSON* diff = build_top_level_diff(s_last_state, root);
  char* diff_json = diff ? cJSON_PrintUnformatted(diff) : nullptr;
  if (diff) cJSON_Delete(diff);

  char* frame = static_cast<char*>(heap_caps_malloc(kFrameCapBytes, MALLOC_CAP_SPIRAM));
  if (!frame) {
    free(snap_json);
    free(diff_json);
    cJSON_Delete(root);
    ESP_LOGE(TAG, "snapshot frame OOM (PSRAM)");
    return;
  }

  bool need_snapshot = true;
  {
    Lock lk;
    need_snapshot = false;
    for (size_t i = 0; i < s_sub_count; ++i) {
      if (s_subs[i].alive && s_subs[i].last_sent_v == 0) {
        need_snapshot = true;
        break;
      }
    }
  }
  if (!diff_json) need_snapshot = true;

  if (need_snapshot) {
    size_t n = format_frame(frame, kFrameCapBytes, "snapshot", version, snap_json);
    if (n == 0) {
      ESP_LOGE(TAG, "snapshot too large for frame buffer (cap=%u); skip",
               (unsigned)kFrameCapBytes);
    } else if (diff_json) {
      send_frame_to_subs(frame, n, version, /*snapshot_only_new=*/true);
    } else {
      send_frame_to_subs(frame, n, version, /*snapshot_only_new=*/false);
    }
  }
  if (diff_json) {
    size_t n = format_frame(frame, kFrameCapBytes, "diff", version, diff_json);
    if (n != 0) {
      send_frame_to_subs(frame, n, version, /*snapshot_only_new=*/false);
    }
  }

  heap_caps_free(frame);
  free(snap_json);
  free(diff_json);

  if (s_last_state) cJSON_Delete(s_last_state);
  s_last_state = root;
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
    const bool streaming = a2dpPlaybackActive() || voiceFeedbackActive();
    vTaskDelay(pdMS_TO_TICKS(streaming ? kBroadcastPeriodStreamingMs : kBroadcastPeriodMs));

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
  //
  // BEFORE checking capacity, prune any stale subscribers. EventSource
  // auto-reconnects fire faster than our 250 ms broadcast tick, so a
  // single browser refresh can stack 2-3 ghost slots that block real
  // tabs from connecting. Pruning here also handles the case where the
  // user closed a tab cleanly: the FIN is observable via MSG_PEEK well
  // before the broadcaster's next send-and-fail would notice.
  {
    Lock lk;
    size_t pruned = prune_dead_subscribers_unlocked();
    if (pruned) {
      ESP_LOGI(TAG, "pruned %u stale subscriber(s) before accept (now count=%u)",
               (unsigned)pruned, (unsigned)s_sub_count);
    }
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

  // CRITICAL: detach BEFORE writing any body. Calling httpd_resp_send_chunk
  // on `req` first puts the response into chunked mode; when this handler
  // returns ESP_OK the httpd cleanup path then emits the terminator chunk
  // (`0\r\n\r\n`) on the socket — even though we tried to detach with
  // async_handler_begin. The browser sees the terminator, treats the SSE
  // response as complete, closes the connection, and EventSource fires its
  // ~1 s auto-reconnect. The recv():ENOTCONN noise immediately after
  // "subscriber added" was the smoking gun: the *just-accepted* socket
  // was already half-closed by the time the worker returned.
  //
  // Pattern matches handle_logs_stream (idf_logs.cpp): set headers,
  // detach, queue the body work for the broadcaster, return ESP_OK.
  // Initial snapshot delivery is now the broadcaster's job and arrives
  // within at most one tick (250 ms) — imperceptible to the SPA.
  httpd_req_t* detached = nullptr;
  if (httpd_req_async_handler_begin(req, &detached) != ESP_OK || !detached) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async begin failed");
  }

  {
    Lock lk;
    // Race-recheck under the lock — another connection might have used the
    // last slot between our pre-accept prune and now. Unlikely in practice
    // but cheap to be safe.
    if (s_sub_count >= kMaxSubscribers) {
      httpd_req_async_handler_complete(detached);
      // Headers were sent but no body chunks; httpd will emit the chunked
      // terminator on cleanup which is fine here (we *want* the client to
      // see the response close).
      return ESP_OK;
    }

    static std::atomic<uint32_t> s_epoch_seq{1};
    s_subs[s_sub_count] = Subscriber{
        detached,
        s_epoch_seq.fetch_add(1, std::memory_order_relaxed),
        // last_sent_v=0 is the "needs initial snapshot" sentinel; the
        // broadcaster's force-publish branch below sends to every
        // subscriber regardless of last_sent_v on the next tick.
        0,
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
    // Force the broadcaster to publish on its next tick so this fresh
    // subscriber gets a snapshot within ≤ kBroadcastPeriodMs even if no
    // state has changed.
    s_force_publish.store(true, std::memory_order_release);
  }
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
