/**
 * Audio Engine – 10-band biquad EQ, PSRAM ring buffer, I2S out.
 */

#include "audio_engine.h"
#include "audio_util.h"
#include <driver/i2s.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cmath>

#ifndef PIN_I2S_BCK
#define PIN_I2S_BCK 26
#endif
#ifndef PIN_I2S_WS
#define PIN_I2S_WS 25
#endif
#ifndef PIN_I2S_DATA
#define PIN_I2S_DATA 22
#endif
static const i2s_port_t I2S_PORT = I2S_NUM_0;

namespace geelen {

static const float EQ_CENTER_HZ[AUDIO_EQ_BANDS] = {
  31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f
};
static const float Q = 1.2f;

void AudioEngine::initBiquads() {
  for (size_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    eq_gains_[i] = 0;
    for (int ch = 0; ch < 2; ch++) {
      state_x1[i][ch] = state_x2[i][ch] = 0.f;
      state_y1[i][ch] = state_y2[i][ch] = 0.f;
    }
  }
  volume_ = 1.f;
  updateBiquadCoefficients();
}

void AudioEngine::updateBiquadCoefficients() {
  float sr = (float)SAMPLE_RATE;
  for (size_t band = 0; band < AUDIO_EQ_BANDS; band++) {
    float g = geelen::gainDbToLinear(eq_gains_[band]);
    float A = sqrtf(g);
    float w0 = 2.f * (float)M_PI * EQ_CENTER_HZ[band] / sr;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.f * Q);
    float a0 = 1.f + alpha / A;
    float b0 = (1.f + alpha * A) / a0;
    float b1 = (-2.f * cosw0) / a0;
    float b2 = (1.f - alpha * A) / a0;
    float a1 = (-2.f * cosw0) / a0;
    float a2 = (1.f - alpha / A) / a0;
    biquad_b0[band] = b0;
    biquad_b1[band] = b1;
    biquad_b2[band] = b2;
    biquad_a1[band] = a1;
    biquad_a2[band] = a2;
  }
}

AudioEngine::AudioEngine()
  : ring_buf_(nullptr), ring_head_(0), ring_tail_(0), ring_cap_(0), running_(false) {
  initBiquads();
}

AudioEngine::~AudioEngine() {
  if (ring_buf_) {
    heap_caps_free(ring_buf_);
    ring_buf_ = nullptr;
  }
}

bool AudioEngine::begin() {
  ring_cap_ = RING_BUF_SAMPLES * 2;
  ring_buf_ = (int16_t*)heap_caps_malloc(RING_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!ring_buf_) {
    ring_buf_ = (int16_t*)malloc(RING_BUF_BYTES);
  }
  if (!ring_buf_) return false;
  ring_head_ = 0;
  ring_tail_ = 0;
  running_ = true;
  return true;
}

void AudioEngine::setEQGains(const int8_t gains[AUDIO_EQ_BANDS]) {
  for (size_t i = 0; i < AUDIO_EQ_BANDS; i++) eq_gains_[i] = gains[i];
  updateBiquadCoefficients();
}

void AudioEngine::setVolume(uint8_t vol_percent) {
  if (vol_percent > 100) vol_percent = 100;
  volume_ = vol_percent / 100.f;
}

void AudioEngine::feedPCM(const uint8_t* data, size_t len_bytes) {
  if (!ring_buf_ || !data) return;
  size_t num_samples = len_bytes / sizeof(int16_t);
  const int16_t* src = (const int16_t*)data;
  for (size_t i = 0; i < num_samples; i++) {
    size_t next = (ring_head_ + 1) % ring_cap_;
    if (next == ring_tail_) break;
    ring_buf_[ring_head_] = src[i];
    ring_head_ = next;
  }
}

void AudioEngine::processSamples(int16_t* buf, size_t num_frames) {
  for (size_t f = 0; f < num_frames; f++) {
    float L = (float)buf[f * 2];
    float R = (float)buf[f * 2 + 1];
    for (size_t band = 0; band < AUDIO_EQ_BANDS; band++) {
      float x0L = L, x0R = R;
      float y0L = biquad_b0[band] * x0L + biquad_b1[band] * state_x1[band][0] + biquad_b2[band] * state_x2[band][0]
                  - biquad_a1[band] * state_y1[band][0] - biquad_a2[band] * state_y2[band][0];
      float y0R = biquad_b0[band] * x0R + biquad_b1[band] * state_x1[band][1] + biquad_b2[band] * state_x2[band][1]
                  - biquad_a1[band] * state_y1[band][1] - biquad_a2[band] * state_y2[band][1];
      state_x2[band][0] = state_x1[band][0]; state_x1[band][0] = x0L;
      state_y2[band][0] = state_y1[band][0]; state_y1[band][0] = y0L;
      state_x2[band][1] = state_x1[band][1]; state_x1[band][1] = x0R;
      state_y2[band][1] = state_y1[band][1]; state_y1[band][1] = y0R;
      L = y0L;
      R = y0R;
    }
    L *= volume_;
    R *= volume_;
    if (L > 32767.f) L = 32767.f;
    if (L < -32768.f) L = -32768.f;
    if (R > 32767.f) R = 32767.f;
    if (R < -32768.f) R = -32768.f;
    buf[f * 2] = (int16_t)L;
    buf[f * 2 + 1] = (int16_t)R;
  }
}

void AudioEngine::process() {
  if (!ring_buf_ || !running_) return;
  const size_t chunk_frames = 128;
  const size_t chunk_samples = chunk_frames * 2;
  int16_t process_buf[chunk_samples];
  size_t to_read = chunk_samples;
  size_t rd = 0;
  while (ring_tail_ != ring_head_ && rd < to_read) {
    process_buf[rd++] = ring_buf_[ring_tail_];
    ring_tail_ = (ring_tail_ + 1) % ring_cap_;
  }
  if (rd == 0) return;
  size_t frames = rd / 2;
  processSamples(process_buf, frames);
  size_t written = 0;
  i2s_write(I2S_PORT, process_buf, rd * sizeof(int16_t), &written, 0);
}

}  // namespace geelen
