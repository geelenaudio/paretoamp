#include "idf_cpu_sampler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <atomic>

namespace geelen_idf {

namespace {

static const char* TAG = "cpu-sampler";

// Two independent counters so idle-hook writes on each core don't touch the
// other core's cache line. std::atomic<uint32_t> gives us single-word atomic
// increments on Xtensa without pulling in a mutex.
static std::atomic<uint32_t> s_idle_count[portNUM_PROCESSORS];
static uint8_t s_cpu_pct[portNUM_PROCESSORS] = {0};

// Calibration baseline: the maximum "idle calls per second" we've ever
// observed across *either* core. Using a single shared peak fixes a subtle
// bug where a core that's never fully idle (e.g. Core 1 pinned to the
// audio task) would calibrate against its own low peak and permanently
// report itself as "busy" even when the audio task is just blocking in
// i2s_channel_write. Both cores are the same Xtensa at the same clock, so
// their idle-hook call rate at 100% idle is identical — calibrating
// against the max we've ever seen on either gives an accurate reading for
// both. Starts at 1 to avoid a divide-by-zero before the first sample
// window closes.
static uint64_t s_idle_peak = 1;

static bool IRAM_ATTR idle_hook_cpu0() {
  s_idle_count[0].fetch_add(1, std::memory_order_relaxed);
  return true;
}

#if portNUM_PROCESSORS > 1
static bool IRAM_ATTR idle_hook_cpu1() {
  s_idle_count[1].fetch_add(1, std::memory_order_relaxed);
  return true;
}
#endif

static void sampler_task(void*) {
  uint32_t prev[portNUM_PROCESSORS] = {0};
  for (int i = 0; i < portNUM_PROCESSORS; ++i) {
    prev[i] = s_idle_count[i].load(std::memory_order_relaxed);
  }
  uint64_t prev_us = esp_timer_get_time();

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    const uint64_t now_us = esp_timer_get_time();
    const uint64_t dt_us = now_us - prev_us;
    prev_us = now_us;
    if (dt_us == 0) continue;

    uint64_t per_s[portNUM_PROCESSORS] = {0};
    for (int i = 0; i < portNUM_PROCESSORS; ++i) {
      const uint32_t cur = s_idle_count[i].load(std::memory_order_relaxed);
      const uint32_t delta = cur - prev[i];
      prev[i] = cur;
      // Scale delta to calls-per-second so the calibration baseline is
      // independent of sampling jitter.
      per_s[i] = (uint64_t)delta * 1000000ULL / dt_us;
      if (per_s[i] > s_idle_peak) s_idle_peak = per_s[i];
    }

    // Apply the *shared* peak to both cores. Any per-second reading below
    // the peak implies that much time spent in non-idle tasks on that core.
    for (int i = 0; i < portNUM_PROCESSORS; ++i) {
      uint64_t pct_idle = per_s[i] * 100ULL / s_idle_peak;
      if (pct_idle > 100) pct_idle = 100;
      s_cpu_pct[i] = (uint8_t)(100u - (uint32_t)pct_idle);
    }
  }
}

}  // namespace

bool cpuSamplerStart() {
  for (int i = 0; i < portNUM_PROCESSORS; ++i) {
    s_idle_count[i].store(0, std::memory_order_relaxed);
  }

  esp_err_t err = esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "idle hook cpu0 registration failed: %d", err);
    return false;
  }
#if portNUM_PROCESSORS > 1
  err = esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "idle hook cpu1 registration failed: %d", err);
    // cpu0 is still useful even if cpu1 fails; don't bail.
  }
#endif

  // Sampler runs on core 0 at a low priority. It doesn't care where it
  // runs, but pinning keeps the scheduler predictable.
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      sampler_task, "cpu-sample", 2048, nullptr, /*prio*/ 1, nullptr,
      /*core*/ 0, MALLOC_CAP_SPIRAM);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "sampler task create failed");
    return false;
  }
  return true;
}

void cpuSamplerRead(uint8_t& core0_pct, uint8_t& core1_pct) {
  core0_pct = s_cpu_pct[0];
#if portNUM_PROCESSORS > 1
  core1_pct = s_cpu_pct[1];
#else
  core1_pct = 0;
#endif
}

}  // namespace geelen_idf
