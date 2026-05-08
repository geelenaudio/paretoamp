// See idf_supervisor.h for the design. Phase 0 brought up the bus + an
// ANY_ID logging handler. Phase 2 adds typed handlers for the BT events
// (`EV_A2DP_CONNECTED`, `EV_A2DP_PAIRING`) so BT host-task callbacks stay
// decoupled from the voice serializer.

#include "idf_supervisor.h"
#include "idf_events.h"
#include "idf_voice_cache.h"
#include "idf_voice_feedback.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <atomic>

namespace geelen_idf {

namespace {

constexpr const char* TAG = "supervisor";

// PIN_REQ debounce: some phones fire it multiple times before the user
// confirms pairing; we only want one "Pairing" prompt per attempt. 5 s
// matches the legacy debounce that lived inside voice_feedback.cpp.
constexpr int64_t kPairingDebounceUs = 5LL * 1000 * 1000;
std::atomic<int64_t> s_last_pairing_us{0};

// Phase 5 — control-plane watchdog.
//
// Updated by `on_any_event` for every event observed on the bus, polled by
// `watchdog_cb` on a periodic esp_timer. The watchdog only complains AFTER
// the bus has gone live at least once (s_last_event_us != 0) so the silent
// pre-boot interval is not flagged. A stall here always means a real
// problem: under normal operation the WiFi / A2DP / state-stream traffic
// keeps the bus busy at sub-second cadence.
constexpr int64_t kWatchdogPollUs   = 10LL * 1000 * 1000;  // poll every 10 s
constexpr int64_t kWatchdogStallUs  = 30LL * 1000 * 1000;  // 30 s of silence
std::atomic<int64_t> s_last_event_us{0};
std::atomic<int64_t> s_last_warning_us{0};
esp_timer_handle_t s_watchdog_timer{nullptr};

// Names matching `geelen_event_id_t`. Indexed directly by id, so the
// table size MUST stay in sync with `EV_COUNT_`. Static asserts at the
// end of this file enforce that.
constexpr const char* kEventNames[] = {
    "EV_NONE",
    "EV_WIFI_UP",
    "EV_WIFI_DOWN",
    "EV_WIFI_AP_FALLBACK",
    "EV_MODE_CHANGE_REQUESTED",
    "EV_MODE_STARTED",
    "EV_MODE_STOPPED",
    "EV_VOICE_SCHEDULE_BOOT",
    "EV_VOICE_PROMPT_FINISHED",
    "EV_VOICE_BOOT_DONE",
    "EV_A2DP_CONNECTED",
    "EV_A2DP_DISCONNECTED",
    "EV_A2DP_PAIRING",
    "EV_SNAP_CONNECTED",
    "EV_SNAP_DISCONNECTED",
    "EV_DLNA_PLAY_REQUESTED",
    "EV_STATE_DIFF",
};

static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) == (size_t)EV_COUNT_,
              "kEventNames out of sync with geelen_event_id_t — add the new "
              "event name and bump EV_COUNT_ together");

// Trace handler — fires for every Geelen event so the live log in the SPA
// shows control-plane activity end-to-end. Cheap enough to leave on; the
// voice / mode handlers below run in parallel for their specific ids.
void on_any_event(void* /*arg*/, esp_event_base_t /*base*/, int32_t id, void* /*data*/) {
  s_last_event_us.store(esp_timer_get_time(), std::memory_order_relaxed);
  const char* name =
      (id >= 0 && id < (int32_t)EV_COUNT_) ? kEventNames[id] : "?";
  ESP_LOGI(TAG, "event id=%d (%s)", (int)id, name);
}

// Phase 5 watchdog timer callback. Runs on the esp_timer task (low prio)
// and only logs; it does NOT reboot or restart anything because the most
// useful signal is "engineer reads the log and sees the stall reason".
// Backs off after warning so we don't spam the log every poll while a
// genuine deadlock keeps the bus quiet. Also emits a periodic heap
// stats line (every 6 polls = 60 s) so memory-pressure regressions are
// visible from the boot UART without rebuilding for heap tracing.
void watchdog_cb(void* /*arg*/) {
  // Periodic heap snapshot — runs whether or not the bus is stalled.
  // Internal-DRAM headroom is the leading indicator for "BT host fails
  // a fixed_queue_new" / "WiFi RX malloc returns null" / "httpd worker
  // RSTs TCP after accept" failure modes. Poll every 6th tick (60 s).
  static int s_poll_count = 0;
  if ((++s_poll_count % 6) == 0) {
    const size_t free_int =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lblk_int =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "heap: int8=%u B free (%u largest); psram=%u B free",
             (unsigned)free_int, (unsigned)lblk_int, (unsigned)free_psram);
  }

  const int64_t last = s_last_event_us.load(std::memory_order_relaxed);
  if (last == 0) return;  // bus has never gone live yet — boot in progress
  const int64_t now = esp_timer_get_time();
  const int64_t gap = now - last;
  if (gap < kWatchdogStallUs) return;

  // Re-warn at most once per stall window so a long deadlock prints
  // a single line every 30 s rather than every poll.
  const int64_t last_warn = s_last_warning_us.load(std::memory_order_relaxed);
  if (last_warn != 0 && now - last_warn < kWatchdogStallUs) return;
  s_last_warning_us.store(now, std::memory_order_relaxed);

  ESP_LOGW(TAG,
           "control-plane watchdog: %lld ms since last event — possible stall",
           (long long)(gap / 1000));
}

// Phase 2: A2DP connected → enqueue the "Bluetooth connected" clip. Done
// here (not in idf_a2dp_sink.cpp) so the BT host task never blocks on the
// voice queue and adding a future "BT disconnected" prompt is a one-line
// edit on the supervisor side.
void on_a2dp_connected(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
  // Phone connect is the most heap-intensive moment in the BT
  // lifecycle: Bluedroid allocates an L2CAP CCB (with its
  // `xmit_hold_q` fixed_queue ~210 B), an SDP CCB, and an AVDTP
  // signalling channel — all from `MALLOC_CAP_INTERNAL`. Log the
  // pool state here so future "phone connect crashes" are
  // diagnosable from the UART stream alone. Matches the pre-init
  // log in `idf_a2dp_sink.cpp`.
  const size_t free_int =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lblk_int =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "A2DP_CONNECTED heap: int8=%u B free, %u B largest",
           (unsigned)free_int, (unsigned)lblk_int);
  voiceFeedbackEnqueueClip(VOICE_CLIP_BLUETOOTH_CONNECTED);
}

// Phase 2: PIN_REQ → pairing prompt with 5 s debounce. Some phones fire
// PIN_REQ several times before the user accepts; without the debounce the
// "Pairing" clip would loop on top of itself.
void on_a2dp_pairing(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
  const int64_t now_us = esp_timer_get_time();
  const int64_t prev = s_last_pairing_us.load(std::memory_order_relaxed);
  if (prev != 0 && now_us - prev < kPairingDebounceUs) return;
  s_last_pairing_us.store(now_us, std::memory_order_relaxed);
  voiceFeedbackEnqueueClip(VOICE_CLIP_PAIRING);
}

// Phase 1: typed log handlers for Wi-Fi transitions. The on_any_event
// trace logger above already prints the bare event id; these unpack the
// payload so the live log reads e.g. `WIFI_UP ip=192.168.1.27` instead
// of just the integer id. Phase 3 (Mode Manager) will hang real
// orchestration off these — DLNA SSDP advertise on UP, mode-manager
// reconciliation on DOWN, etc.
void on_wifi_up(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* data) {
  if (!data) {
    ESP_LOGI(TAG, "WIFI_UP (no payload)");
    return;
  }
  const auto* p = static_cast<const ev_wifi_up_t*>(data);
  // Address is stored network-byte-order (LE on ESP32). Print octet-major
  // form so a glance at the log matches what `ip a` shows on the router.
  const uint32_t ip = p->ip4_be;
  ESP_LOGI(TAG, "WIFI_UP ip=%u.%u.%u.%u",
           (unsigned)((ip >> 0) & 0xff),
           (unsigned)((ip >> 8) & 0xff),
           (unsigned)((ip >> 16) & 0xff),
           (unsigned)((ip >> 24) & 0xff));
}

void on_wifi_down(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* data) {
  if (!data) {
    ESP_LOGW(TAG, "WIFI_DOWN (no payload)");
    return;
  }
  const auto* p = static_cast<const ev_wifi_down_t*>(data);
  ESP_LOGW(TAG, "WIFI_DOWN reason=%u", (unsigned)p->reason);
}

void on_wifi_ap_fallback(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
  ESP_LOGW(TAG, "WIFI_AP_FALLBACK — captive AP active for reconfiguration");
}

}  // namespace

bool supervisorBegin() {
  if (eventLoop() == nullptr) {
    ESP_LOGE(TAG, "supervisor: event loop creation failed");
    return false;
  }
  // Trace logger fires for every event; per-id handlers register on top so
  // ESP_LOGI for the trace + the typed handler both run.
  esp_err_t e = eventRegister(ESP_EVENT_ANY_ID, &on_any_event, /*arg=*/nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register ANY handler failed: %s", esp_err_to_name(e));
    return false;
  }

  e = eventRegister(EV_A2DP_CONNECTED, &on_a2dp_connected, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register EV_A2DP_CONNECTED failed: %s", esp_err_to_name(e));
    return false;
  }
  e = eventRegister(EV_A2DP_PAIRING, &on_a2dp_pairing, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register EV_A2DP_PAIRING failed: %s", esp_err_to_name(e));
    return false;
  }

  // Phase 1 — Wi-Fi transitions. Currently log-only; actual reconnection
  // logic stays inside WifiManager (it owns the radio + ladder timers).
  // The supervisor handlers exist so `EV_WIFI_*` is observable from the
  // SPA log stream without grepping for the bare integer id, and so
  // future modules (DLNA SSDP, Mode Manager) have a single hook point.
  e = eventRegister(EV_WIFI_UP, &on_wifi_up, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register EV_WIFI_UP failed: %s", esp_err_to_name(e));
    return false;
  }
  e = eventRegister(EV_WIFI_DOWN, &on_wifi_down, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register EV_WIFI_DOWN failed: %s", esp_err_to_name(e));
    return false;
  }
  e = eventRegister(EV_WIFI_AP_FALLBACK, &on_wifi_ap_fallback, nullptr);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "supervisor: register EV_WIFI_AP_FALLBACK failed: %s", esp_err_to_name(e));
    return false;
  }

  // Phase 5 watchdog: kick off the periodic stall checker. Failure to
  // arm the timer is non-fatal — the bus still works, we just lose the
  // "30 s silence" early warning.
  if (!s_watchdog_timer) {
    esp_timer_create_args_t args{};
    args.callback = &watchdog_cb;
    args.arg = nullptr;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "geelen_wd";
    esp_timer_handle_t th = nullptr;
    if (esp_timer_create(&args, &th) == ESP_OK) {
      s_watchdog_timer = th;
      esp_timer_start_periodic(th, kWatchdogPollUs);
    } else {
      ESP_LOGW(TAG, "supervisor: watchdog timer create failed (non-fatal)");
    }
  }

  ESP_LOGI(TAG, "supervisor up (Phases 1 + 2 + 5: Wi-Fi + A2DP + watchdog active)");
  return true;
}

}  // namespace geelen_idf
