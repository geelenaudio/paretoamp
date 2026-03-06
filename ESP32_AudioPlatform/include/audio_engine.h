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
constexpr size_t RING_BUF_SAMPLES = 4096;
constexpr size_t RING_BUF_BYTES = RING_BUF_SAMPLES * sizeof(int16_t) * 2;

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  bool begin();
  void setEQGains(const int8_t gains[AUDIO_EQ_BANDS]);
  void setVolume(uint8_t vol_percent);

  void feedPCM(const uint8_t* data, size_t len_bytes);
  void process();

  bool isRunning() const { return running_; }

 private:
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
};

}  // namespace geelen

#endif  // GEELEN_AUDIO_ENGINE_H
