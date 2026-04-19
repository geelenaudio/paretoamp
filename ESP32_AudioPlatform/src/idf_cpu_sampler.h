// Lightweight per-core CPU load sampler.
//
// ESP-IDF's built-in FreeRTOS runtime stats (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
// are accurate but not free: they tax every context switch and allocate
// TCB-sized scratch for uxTaskGetSystemState(). For a status-light use case
// (one-decimal CPU% for the overview card) we want something cheaper.
//
// Approach: register a FreeRTOS idle hook on each core that increments a
// counter every time the IDLE task runs. Once a second, snapshot the delta,
// translate it to "idle calls per second", and compare against the busiest
// idle rate we've ever seen (our 0%-load calibration baseline). CPU load is
// `100 - (current / peak) * 100`.
//
// The calibration baseline auto-raises itself whenever it sees a higher
// idle rate, so the reading converges to "true 0%" within a few seconds of
// the system going quiet (e.g. during the first few seconds of boot or
// when audio is paused).
//
// Accuracy is ±a few percent, which is plenty for a dashboard reading.
#pragma once

#include <cstdint>

namespace geelen_idf {

// Registers the idle hooks and spawns the 1 Hz sampler task. Safe to call
// exactly once during boot; returns false if registration fails.
bool cpuSamplerStart();

// Reads the latest per-core load in percent (0..100). Core 1 returns 0 on
// single-core targets. Non-blocking; cheap enough to call from HTTP handlers.
void cpuSamplerRead(uint8_t& core0_pct, uint8_t& core1_pct);

}  // namespace geelen_idf
