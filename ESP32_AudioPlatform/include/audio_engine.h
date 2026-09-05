// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Audio Engine — single-layer parametric EQ, PSRAM ring buffer, I2S output.
// Feed PCM via feedPCM(); call process() to drain the ring → EQ → I2S.
//
// EQ backend selection (compile-time via HAL_HAS_HW_PEQ):
//   - Amped (HAL_HAS_HW_PEQ=0): the AudioEngine runs the full cascade in
//     software on the audio task. Bands beyond `maxBands()` (=10) are ignored.
//     Always one ganged curve: shared coefficients, independent L/R delay
//     lines. Do not add a second software cascade — 10-band stereo already
//     saturates Core 1 on Amped-WROVER (see docs/eq.md "Channel mode").
//   - Louder (HAL_HAS_HW_PEQ=1): the AudioEngine can be told to skip its own
//     cascade because the DAC applied the biquads in hardware. When the HW
//     path is active, `setEqCurve(..., use_hw = true)` sets `cascade_.enabled
//     = false` so the software math stays out of the way. Split L/R lives
//     in TAS Book 0xAA, not here.

#ifndef GEELEN_AUDIO_ENGINE_H
#define GEELEN_AUDIO_ENGINE_H

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "eq_coeffs.h"
#include "eq_types.h"

// Set to 1 (or pass -DVERBOSE_AUDIO_LOG=1) to surface fine-grained gating logs
// for the boot-prompt path: prefill waits in `AudioEngine::process()`, MP3
// streamer back-pressure release in `pumpOnce`, and `bufferedFrames` snapshots
// around the voice-feedback drain. Off by default — these fire many times per
// second and would otherwise drown the normal log stream.
#ifndef VERBOSE_AUDIO_LOG
#define VERBOSE_AUDIO_LOG 0
#endif

namespace geelen {

// Storage size. See `HAL_EQ_MAX_BANDS_ACTIVE` for the per-board runtime cap.
constexpr size_t AUDIO_EQ_BANDS = EQ_MAX_BANDS;

constexpr int SAMPLE_RATE = 44100;
constexpr size_t RING_BUF_SAMPLES = (size_t)SAMPLE_RATE * 2;
constexpr size_t RING_BUF_BYTES = RING_BUF_SAMPLES * sizeof(int16_t) * 2;

/** PCM buffered before first I2S chunk. 1 s made voice/boot clips wait too long
 *  for first sound; 500 ms is still ample for A2DP/DLNA jitter on this ring. */
constexpr uint32_t AUDIO_PREFILL_MS = 500;
constexpr uint32_t AUDIO_LOW_WATER_MS = 300;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  bool begin();
  void setI2SChannel(void* tx_handle);
  void setEQGains(const int8_t gains[AUDIO_EQ_BANDS]);
  /**
   * Load a new PEQ curve. `active_band_count` is the number of `bands` to
   * treat as active — extras are ignored. `enabled` toggles the software
   * cascade; Louder callers pass `false` here when the DAC is doing the
   * biquads in hardware.
   */
  void setEqCurve(const EqBand* bands, uint8_t active_band_count, bool enabled);
  enum class EqBackend : uint8_t { Software = 0, Hardware = 1 };
  void setEqBackend(EqBackend b);
  EqBackend eqBackend() const { return eq_backend_; }
  /** Runtime cap on the software cascade (=HAL_EQ_MAX_BANDS_ACTIVE). */
  uint8_t maxBands() const;
  void setHeadroomDb(float db);
  /** Peak gain of enabled EQ (dB); thread-safe, uses a cache invalidated by `setEqCurve`. */
  float computeEqPeakGainDb() const;
  void setVolume(uint8_t vol_percent);
  void setVolumeLR(uint8_t l_pct, uint8_t r_pct);
  void setMuted(bool muted);
  void setChannels(bool l_on, bool r_on, bool mono);

  void feedPCM(const uint8_t* data, size_t len_bytes);
  bool process();

  bool isRunning() const { return running_; }
  /** True while `process()` is actively draining the ring to I2S — i.e. past
   *  prefill and not currently in a rebuffer cycle. Producers (e.g. the MP3
   *  streamer's `pumpOnce` back-pressure spin) read this to decide whether
   *  to throttle: throttling before the engine has ever crossed prefill can
   *  livelock the boot-prompt path at the prefill threshold. */
  bool isOutputActive() const { return playing_.load(std::memory_order_relaxed); }
  size_t bufferedFrames() const;

  /** Post-DSP peak / RMS (dBFS-ish), for `/api/system/metrics` and the test page. */
  void readOutputMeters(float& peak_l_db, float& peak_r_db, float& rms_l_db, float& rms_r_db) const;

 private:
  void updateOutputMeters(const int16_t* buf, size_t num_frames);
  size_t ringFillSamples() const;
  /** Like `ringFillSamples()` but uses relaxed atomic loads (SPSC-safe). */
  size_t ringFillSamplesLocked() const;
  void initBiquads();
  void rebuildCascade();
  void processSamples(int16_t* buf, size_t num_frames);

  // One coefficient list, two delay lines ([stage][ch]). Ganged on purpose:
  // Amped has no second curve. Adding stages_L / stages_R is a product
  // decision against docs/eq.md, not a missing array.
  struct Cascade {
    EqBand bands[AUDIO_EQ_BANDS];
    uint8_t active_band_count;
    EqStage stages[EQ_MAX_STAGES_PER_LAYER];
    uint8_t stage_count;
    float zx1[EQ_MAX_STAGES_PER_LAYER][2];
    float zx2[EQ_MAX_STAGES_PER_LAYER][2];
    float zy1[EQ_MAX_STAGES_PER_LAYER][2];
    float zy2[EQ_MAX_STAGES_PER_LAYER][2];
    bool enabled;
  };
  Cascade cascade_;
  /** Serializes EQ structure updates vs `processSamples`. */
  void* eq_mutex_{nullptr};
  mutable bool eq_peak_valid_{false};
  mutable float eq_peak_db_{0.f};

  float computeEqPeakGainDbUnlocked() const;

  float headroom_linear_;
  float volume_;
  float volume_l_;
  float volume_r_;
  bool muted_;
  bool l_on_;
  bool r_on_;
  bool mono_;

  int16_t* ring_buf_;
  std::atomic<size_t> ring_head_{0};
  std::atomic<size_t> ring_tail_{0};
  size_t ring_cap_;
  bool running_;
  void* i2s_tx_handle_;
  /** Serializes multiple PCM producers (A2DP, Snapcast, MP3, voice) against
   *  each other. The I2S consumer never takes this mutex — it uses the
   *  lock-free SPSC head/tail atomics. */
  void* producer_mutex_{nullptr};
  EqBackend eq_backend_{EqBackend::Software};

  /** Atomic so producers (`AudioEngine::isOutputActive()` callers on other
   *  tasks) can observe the prefill→playing transition without taking
   *  `producer_mutex_`. Writers still hold the mutex; the atomic just removes
   *  the data race on the unlocked read path. */
  std::atomic<bool> playing_;
  uint32_t underruns_;
  uint32_t overflows_;
  /** Counts only real `i2s_channel_write` errors (`ESP_ERR_*`), not benign DMA backpressure. */
  uint32_t i2s_tx_errors_;
  /** Monotonic µs inside `i2s_channel_write` (mostly blocked on DMA — expected pacing, not CPU burn). */
  uint64_t i2s_blocked_us_total_;
  /** Snapshot of `i2s_blocked_us_total_` at the last per-second stats line. */
  uint64_t last_stats_blocked_us_;
  unsigned long last_stats_ms_;
  uint64_t last_contention_log_us_;
  uint64_t last_i2s_backpressure_log_us_;
  /** Smoothed inter-chunk true-peak limiter (avoids zipper/crackle from per-chunk uniform scale steps). */
  float intersample_limiter_gain_;

  /** Linear 0..1 peak hold with per-chunk decay (updated from `process()` after EQ). */
  mutable std::atomic<float> meter_peak_lin_l_{0.f};
  mutable std::atomic<float> meter_peak_lin_r_{0.f};
  /** Mean-square linear 0..1, EMA-smoothed per chunk. */
  mutable std::atomic<float> meter_rms_ms_l_{0.f};
  mutable std::atomic<float> meter_rms_ms_r_{0.f};
};

}  // namespace geelen

#endif
