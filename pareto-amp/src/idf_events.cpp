// Geelen control-plane event bus. See idf_events.h for the design.

#include "idf_events.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

ESP_EVENT_DEFINE_BASE(GEELEN_EVENTS);

namespace geelen_idf {

namespace {

constexpr const char* TAG = "events";

// Lazily initialised on the first `eventLoop()` call; cached for the
// lifetime of the firmware. Atomic so concurrent first-callers from
// different tasks (e.g. supervisorBegin and idf_main during boot race)
// don't both create a loop.
std::atomic<esp_event_loop_handle_t> s_loop{nullptr};

}  // namespace

esp_event_loop_handle_t eventLoop() {
  esp_event_loop_handle_t cur = s_loop.load(std::memory_order_acquire);
  if (cur) return cur;

  // Sized for the boot burst: WIFI_UP + MODE_CHANGE + VOICE_SCHEDULE_BOOT
  // + the boot pair finishing + an A2DP/Snap connection notification can
  // arrive within a few hundred ms during a normal cold boot. 32 slots
  // keeps producers from blocking on `esp_event_post_to` even if the
  // supervisor is briefly preempted by httpd or audio_task.
  esp_event_loop_args_t args{};
  args.queue_size = 32;
  args.task_name = "geelen-ev";
  // Stack is allocated from internal DRAM by `esp_event_loop_create`
  // (no PSRAM caps option exposed by the IDF API), so it directly
  // competes with Bluedroid's L2CAP/SDP CCB allocations during phone
  // connect. 3 KiB is sized for: dispatched payload (≤256 B for the
  // DLNA URI case), ESP_LOGI vsnprintf scratch (~256 B), small
  // handler locals, FreeRTOS overhead. Empirically fine for the
  // Phase 1/2/5 handlers (Wi-Fi log, A2DP voice enqueue, watchdog
  // timer touch) — tighter than the IDF SSE-worker default but those
  // call cJSON_Print on the stack which we never do here.
  args.task_stack_size = 3072;
  // Below `httpd` (10) and `audio_task` (20) so neither is starved by a
  // long handler chain; above `voice` (3) and `cpu-sampler` (1) so
  // orchestration runs ahead of the workers it commands.
  args.task_priority = 8;
  // Co-located with `audio_task` on Core 1. Wi-Fi/BT/lwip own Core 0;
  // posting from those tasks goes through the FreeRTOS queue (cross-core
  // is fine), but handlers themselves run here on the audio side so
  // mode-change-driven engine reconfigurations execute on the same core
  // as the consumer.
  args.task_core_id = 1;

  esp_event_loop_handle_t h = nullptr;
  esp_err_t e = esp_event_loop_create(&args, &h);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "esp_event_loop_create failed: %s", esp_err_to_name(e));
    return nullptr;
  }

  esp_event_loop_handle_t expected = nullptr;
  if (!s_loop.compare_exchange_strong(expected, h, std::memory_order_acq_rel)) {
    // Lost a race: another caller already published a loop. Tear ours
    // down and use theirs so we keep exactly one loop for the firmware
    // (handlers register against a specific handle).
    esp_event_loop_delete(h);
    return s_loop.load(std::memory_order_acquire);
  }

  ESP_LOGI(TAG, "geelen event loop created (queue=%d, prio=%d, core=%d)",
           (int)args.queue_size, (int)args.task_priority, (int)args.task_core_id);
  return h;
}

esp_err_t eventPost(geelen_event_id_t id, const void* data, size_t len) {
  esp_event_loop_handle_t h = eventLoop();
  if (!h) return ESP_ERR_INVALID_STATE;
  return esp_event_post_to(h, GEELEN_EVENTS, (int32_t)id, data, len, portMAX_DELAY);
}

esp_err_t eventPostFromIsr(geelen_event_id_t id, const void* data, size_t len,
                           BaseType_t* hp_task_woken) {
  // Never create the loop from ISR; callers are responsible for ensuring
  // `eventLoop()` has been called from a task context first (the supervisor
  // does this at boot via `supervisorBegin`).
  esp_event_loop_handle_t h = s_loop.load(std::memory_order_acquire);
  if (!h) return ESP_ERR_INVALID_STATE;
  return esp_event_isr_post_to(h, GEELEN_EVENTS, (int32_t)id, data, len, hp_task_woken);
}

esp_err_t eventRegister(int32_t id, esp_event_handler_t handler, void* arg) {
  esp_event_loop_handle_t h = eventLoop();
  if (!h) return ESP_ERR_INVALID_STATE;
  return esp_event_handler_register_with(h, GEELEN_EVENTS, id, handler, arg);
}

}  // namespace geelen_idf
