/**
 * Audio Engine – 10-band EQ, PSRAM ring buffer, I2S output.
 * Feed PCM via feedPCM(); call process() to drain buffer → EQ → I2S.
 */

#ifndef GEELEN_AUDIO_ENGINE_H
#define GEELEN_AUDIO_ENGINE_H

#include <cstdint>
#include <cstddef>

namespace geelen {

constexpr size_t AUDIO_EQ_BANDS = 10;
constexpr int SAMPLE_RATE = 44100;
// 2 seconds of stereo 16-bit PCM at 44.1kHz (frames, not int16 samples)
constexpr size_t RING_BUF_SAMPLES = (size_t)SAMPLE_RATE * 2;
constexpr size_t RING_BUF_BYTES = RING_BUF_SAMPLES * sizeof(int16_t) * 2;

// Buffering strategy:
// - Prefill before starting playback to absorb BT/WiFi scheduling jitter
// - If we drop too low, pause and rebuffer
constexpr uint32_t AUDIO_PREFILL_MS = 400;
constexpr uint32_t AUDIO_LOW_WATER_MS = 120;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  bool begin();
  /** Set I2S TX channel handle (new driver). Pass from main after i2s_channel_init_std_mode. */
  void setI2SChannel(void* tx_handle);
  void setEQGains(const int8_t gains[AUDIO_EQ_BANDS]);
  void setVolume(uint8_t vol_percent);

  void feedPCM(const uint8_t* data, size_t len_bytes);
  void process();

  bool isRunning() const { return running_; }
  /** Current buffered audio (frames at 44.1kHz stereo). Useful for drift control. */
  size_t bufferedFrames() const;

 private:
  size_t ringFillSamples() const;
  void initBiquads();
  void updateBiquadCoefficients();
  void processSamples(int16_t* buf, size_t num_frames);

  float biquad_b0[AUDIO_EQ_BANDS], biquad_b1[AUDIO_EQ_BANDS], biquad_b2[AUDIO_EQ_BANDS];
  float biquad_a1[AUDIO_EQ_BANDS], biquad_a2[AUDIO_EQ_BANDS];
  float state_x1[AUDIO_EQ_BANDS][2], state_x2[AUDIO_EQ_BANDS][2];
  float state_y1[AUDIO_EQ_BANDS][2], state_y2[AUDIO_EQ_BANDS][2];
  int8_t eq_gains_[AUDIO_EQ_BANDS];
  float volume_;

  int16_t* ring_buf_;
  volatile size_t ring_head_;
  volatile size_t ring_tail_;
  size_t ring_cap_;
  bool running_;
  void* i2s_tx_handle_;  /* i2s_chan_handle_t from driver/i2s_std.h */

  bool playing_;
  uint32_t underruns_;
  uint32_t overflows_;
  unsigned long last_stats_ms_;
};

}  // namespace geelen

#endif  // GEELEN_AUDIO_ENGINE_H
