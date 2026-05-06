/**
 * Audio Engine – parametric EQ (multi-stage), PSRAM ring buffer, I2S output.
 * Feed PCM via feedPCM(); call process() to drain buffer → EQ → I2S.
 */

#ifndef GEELEN_AUDIO_ENGINE_H
#define GEELEN_AUDIO_ENGINE_H

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "eq_coeffs.h"
#include "eq_types.h"

namespace geelen {

constexpr size_t AUDIO_EQ_BANDS = EQ_MAX_BANDS;
constexpr size_t AUDIO_EQ_LAYERS = 2;
constexpr int EQ_LAYER_CALIBRATION = 0;
constexpr int EQ_LAYER_PREFERENCE = 1;

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
  /** `active_band_count` in 1..AUDIO_EQ_BANDS; only first N bands are used. */
  void setEqCurve(int layer, const EqBand bands[AUDIO_EQ_BANDS], uint8_t active_band_count, bool enabled);
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
  size_t bufferedFrames() const;

  /** Post-DSP peak / RMS (dBFS-ish), for `/api/system/metrics` and the test page. */
  void readOutputMeters(float& peak_l_db, float& peak_r_db, float& rms_l_db, float& rms_r_db) const;

 private:
  void updateOutputMeters(const int16_t* buf, size_t num_frames);
  size_t ringFillSamples() const;
  /** Like `ringFillSamples()` but caller must hold `feed_mutex_` (or be single-threaded). */
  size_t ringFillSamplesLocked() const;
  void initBiquads();
  void rebuildCascade(int layer);
  void processSamples(int16_t* buf, size_t num_frames);

  // (rollback IirDelaySnapshot was used by the old "retry on I2S timeout" path;
  // removed in the I2S/Core‑1 fix — `processSamples` now always commits its
  // result and the I2S write is one blocking call paced by DMA.)

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
  Cascade cascades_[AUDIO_EQ_LAYERS];
  /** Serializes EQ structure updates vs `processSamples` (same core as feed is still racy with A2DP). */
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
  volatile size_t ring_head_;
  volatile size_t ring_tail_;
  size_t ring_cap_;
  bool running_;
  void* i2s_tx_handle_;
  /** `SemaphoreHandle_t` — serializes multi-writer `feedPCM` (A2DP, Snapcast, MP3). */
  void* feed_mutex_{nullptr};

  bool playing_;
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
