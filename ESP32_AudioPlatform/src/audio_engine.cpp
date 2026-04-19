/**
 * Audio Engine – 10-band biquad EQ, PSRAM ring buffer, I2S out.
 */

#include "audio_engine.h"
#include "audio_util.h"
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <cstdarg>
#include <cstdio>
#ifndef ARDUINO
#include <esp_log.h>
#endif
#include <cmath>

namespace geelen {

static unsigned long nowMs() {
#ifdef ARDUINO
  return millis();
#else
  return (unsigned long)(esp_timer_get_time() / 1000ULL);
#endif
}

static void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#ifdef ARDUINO
  Serial.vprintf(fmt, ap);
#else
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  // Trim trailing newline handling is caller-defined; keep as-is.
  ESP_LOGI("Audio", "%s", buf);
#endif
  va_end(ap);
}

// Legacy graphic-EQ centre frequencies. Used by setEQGains() and by the
// default-calibration initialiser so the "untouched" curve matches what
// the device used to ship with.
static const float EQ_CENTER_HZ[AUDIO_EQ_BANDS] = {
  31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f
};
static constexpr float DEFAULT_Q = 1.2f;

void AudioEngine::initBiquads() {
  for (int layer = 0; layer < (int)AUDIO_EQ_LAYERS; ++layer) {
    Cascade& c = cascades_[layer];
    c.enabled = false;
    for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) {
      c.bands[i] = { EQ_CENTER_HZ[i], 0.f, DEFAULT_Q };
      for (int ch = 0; ch < 2; ++ch) {
        c.x1[i][ch] = c.x2[i][ch] = 0.f;
        c.y1[i][ch] = c.y2[i][ch] = 0.f;
      }
    }
    rebuildCascade(layer);
  }
  headroom_linear_ = 1.f;
  volume_ = 1.f;
  volume_l_ = 1.f;
  volume_r_ = 1.f;
  muted_ = false;
  l_on_ = true;
  r_on_ = true;
  mono_ = false;
}

void AudioEngine::rebuildCascade(int layer) {
  if (layer < 0 || layer >= (int)AUDIO_EQ_LAYERS) return;
  Cascade& c = cascades_[layer];
  const float sr = (float)SAMPLE_RATE;
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) {
    // RBJ peaking-EQ biquad. Same math as the legacy EQ; here the
    // caller supplies per-band frequency, gain, and Q instead of a
    // fixed-frequency graphic EQ.
    float freq = c.bands[i].freq_hz;
    float gain = c.bands[i].gain_db;
    float q    = c.bands[i].q;
    // Clamp hard — coefficient math is undefined outside these ranges.
    if (freq < 20.f) freq = 20.f;
    if (freq > sr * 0.45f) freq = sr * 0.45f;  // below Nyquist margin
    if (q < 0.1f)  q = 0.1f;
    if (q > 10.f)  q = 10.f;
    const float A = powf(10.f, gain / 40.f);  // sqrt(linear)
    const float w0 = 2.f * (float)M_PI * freq / sr;
    const float cosw0 = cosf(w0);
    const float alpha = sinf(w0) / (2.f * q);
    const float a0 = 1.f + alpha / A;
    c.b0[i] = (1.f + alpha * A) / a0;
    c.b1[i] = (-2.f * cosw0) / a0;
    c.b2[i] = (1.f - alpha * A) / a0;
    c.a1[i] = (-2.f * cosw0) / a0;
    c.a2[i] = (1.f - alpha / A) / a0;
  }
}

AudioEngine::AudioEngine()
  : ring_buf_(nullptr),
    ring_head_(0),
    ring_tail_(0),
    ring_cap_(0),
    running_(false),
    i2s_tx_handle_(nullptr),
    playing_(false),
    underruns_(0),
    overflows_(0),
    last_stats_ms_(0) {
  initBiquads();
}

size_t AudioEngine::ringFillSamples() const {
  size_t h = ring_head_;
  size_t t = ring_tail_;
  if (h >= t) return h - t;
  return ring_cap_ - (t - h);
}

size_t AudioEngine::bufferedFrames() const {
  // Stereo frames.
  return ringFillSamples() / 2;
}

void AudioEngine::setI2SChannel(void* tx_handle) {
  i2s_tx_handle_ = tx_handle;
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
  playing_ = false;
  underruns_ = 0;
  overflows_ = 0;
  last_stats_ms_ = nowMs();
  running_ = true;
  return true;
}

void AudioEngine::setEQGains(const int8_t gains[AUDIO_EQ_BANDS]) {
  Cascade& c = cascades_[EQ_LAYER_CALIBRATION];
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) {
    c.bands[i].freq_hz = EQ_CENTER_HZ[i];
    c.bands[i].gain_db = (float)gains[i];
    c.bands[i].q       = DEFAULT_Q;
  }
  c.enabled = true;
  rebuildCascade(EQ_LAYER_CALIBRATION);
}

void AudioEngine::setEqCurve(int layer, const EqBand bands[AUDIO_EQ_BANDS], bool enabled) {
  if (layer < 0 || layer >= (int)AUDIO_EQ_LAYERS) return;
  Cascade& c = cascades_[layer];
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) c.bands[i] = bands[i];
  c.enabled = enabled;
  rebuildCascade(layer);
}

void AudioEngine::setHeadroomDb(float db) {
  if (db > 0.f) db = 0.f;  // only ever attenuating
  // Guard against absurd values that would crash or deafen.
  if (db < -60.f) db = -60.f;
  headroom_linear_ = powf(10.f, db / 20.f);
}

float AudioEngine::computeEqPeakGainDb() const {
  // Sweep 256 log-spaced points from 20 Hz to 20 kHz and accumulate the
  // product of per-band |H(f)| across both enabled cascades. Tracking the
  // max gives us the worst-case headroom we need to pre-attenuate so the
  // cascade's output stays at ≤ 0 dB regardless of input. 256 points is
  // plenty to resolve a Q=10 peak at 20 kHz (-> bandwidth ~2 kHz) while
  // still being cheap enough to run on any EQ change.
  constexpr int N = 256;
  const float sr = (float)SAMPLE_RATE;
  const float f_lo = 20.f;
  const float f_hi = 20000.f;
  // Use std::log explicitly — the file's own `logf(fmt, ...)` printer
  // shadows the <cmath> free function.
  const float log_lo = std::log(f_lo);
  const float log_hi = std::log(f_hi);

  float peak = 1.f;
  for (int i = 0; i < N; ++i) {
    const float f = expf(log_lo + (log_hi - log_lo) * (float)i / (float)(N - 1));
    const float w = 2.f * (float)M_PI * f / sr;
    const float cw = cosf(w);
    const float c2w = cosf(2.f * w);
    const float sw = sinf(w);
    const float s2w = sinf(2.f * w);

    float mag_product = 1.f;
    for (int layer = 0; layer < (int)AUDIO_EQ_LAYERS; ++layer) {
      const Cascade& c = cascades_[layer];
      if (!c.enabled) continue;
      for (size_t b = 0; b < AUDIO_EQ_BANDS; ++b) {
        // |H(e^jw)| = |b0 + b1 e^-jw + b2 e^-j2w| / |1 + a1 e^-jw + a2 e^-j2w|
        const float num_re = c.b0[b] + c.b1[b] * cw + c.b2[b] * c2w;
        const float num_im =           -c.b1[b] * sw - c.b2[b] * s2w;
        const float den_re = 1.f      + c.a1[b] * cw + c.a2[b] * c2w;
        const float den_im =           -c.a1[b] * sw - c.a2[b] * s2w;
        const float num = sqrtf(num_re * num_re + num_im * num_im);
        const float den = sqrtf(den_re * den_re + den_im * den_im);
        if (den > 1e-12f) mag_product *= num / den;
      }
    }
    if (mag_product > peak) peak = mag_product;
  }
  return 20.f * log10f(peak);
}

void AudioEngine::setVolume(uint8_t vol_percent) {
  if (vol_percent > 100) vol_percent = 100;
  volume_ = vol_percent / 100.f;
  // Keep per-channel in sync for legacy callers — the new UI uses
  // setVolumeLR() directly and will overwrite these shortly afterwards.
  volume_l_ = volume_r_ = volume_;
}

void AudioEngine::setVolumeLR(uint8_t l_pct, uint8_t r_pct) {
  if (l_pct > 100) l_pct = 100;
  if (r_pct > 100) r_pct = 100;
  volume_l_ = l_pct / 100.f;
  volume_r_ = r_pct / 100.f;
  // Legacy `volume_` tracks the max channel so `getVolume()`-style callers
  // see something sensible.
  volume_ = volume_l_ > volume_r_ ? volume_l_ : volume_r_;
}

void AudioEngine::setMuted(bool muted) {
  muted_ = muted;
}

void AudioEngine::setChannels(bool l_on, bool r_on, bool mono) {
  l_on_ = l_on;
  r_on_ = r_on;
  mono_ = mono;
}

void AudioEngine::feedPCM(const uint8_t* data, size_t len_bytes) {
  if (!ring_buf_ || !data) return;
  size_t num_samples = len_bytes / sizeof(int16_t);
  const int16_t* src = (const int16_t*)data;
  for (size_t i = 0; i < num_samples; i++) {
    size_t next = (ring_head_ + 1) % ring_cap_;
    if (next == ring_tail_) {
      overflows_++;
      break;
    }
    ring_buf_[ring_head_] = src[i];
    ring_head_ = next;
  }
}

void AudioEngine::processSamples(int16_t* buf, size_t num_frames) {
  // Snapshot the control state once per block. API mutators run on another
  // task but each write is to a word-sized primitive, so a torn read costs
  // at most one block of wrong gain — inaudible.
  const bool  mono   = mono_;
  const bool  muted  = muted_;
  const bool  lon    = l_on_;
  const bool  ron    = r_on_;
  const float gL     = muted ? 0.f : volume_l_;
  const float gR     = muted ? 0.f : volume_r_;

  const float hr = headroom_linear_;

  for (size_t f = 0; f < num_frames; f++) {
    float L = (float)buf[f * 2];
    float R = (float)buf[f * 2 + 1];
    // Two cascades (calibration, preference). Both always run their
    // state-update math so biquad history stays coherent when enables
    // flip at runtime — a disabled cascade simply feeds output = input.
    for (int layer = 0; layer < (int)AUDIO_EQ_LAYERS; ++layer) {
      Cascade& c = cascades_[layer];
      if (!c.enabled) continue;
      for (size_t band = 0; band < AUDIO_EQ_BANDS; ++band) {
        float x0L = L, x0R = R;
        float y0L = c.b0[band] * x0L + c.b1[band] * c.x1[band][0] + c.b2[band] * c.x2[band][0]
                    - c.a1[band] * c.y1[band][0] - c.a2[band] * c.y2[band][0];
        float y0R = c.b0[band] * x0R + c.b1[band] * c.x1[band][1] + c.b2[band] * c.x2[band][1]
                    - c.a1[band] * c.y1[band][1] - c.a2[band] * c.y2[band][1];
        c.x2[band][0] = c.x1[band][0]; c.x1[band][0] = x0L;
        c.y2[band][0] = c.y1[band][0]; c.y1[band][0] = y0L;
        c.x2[band][1] = c.x1[band][1]; c.x1[band][1] = x0R;
        c.y2[band][1] = c.y1[band][1]; c.y1[band][1] = y0R;
        L = y0L;
        R = y0R;
      }
    }
    L *= hr;
    R *= hr;
    if (mono) {
      // Preserve energy with a −3 dB sum (arithmetic average) rather than
      // clipping-prone straight add. Feed the average to both channels so
      // channel-specific gain / on-off can still silence one side.
      const float M = (L + R) * 0.5f;
      L = M;
      R = M;
    }
    float outL = lon ? L * gL : 0.f;
    float outR = ron ? R * gR : 0.f;
    if (outL > 32767.f) outL = 32767.f;
    if (outL < -32768.f) outL = -32768.f;
    if (outR > 32767.f) outR = 32767.f;
    if (outR < -32768.f) outR = -32768.f;
    buf[f * 2]     = (int16_t)outL;
    buf[f * 2 + 1] = (int16_t)outR;
  }
}

bool AudioEngine::process() {
  if (!ring_buf_ || !running_) return false;
  if (!i2s_tx_handle_) return false;

  const size_t fill_samples = ringFillSamples();
  const size_t fill_frames = fill_samples / 2;
  const size_t prefill_frames = (size_t)((uint64_t)SAMPLE_RATE * AUDIO_PREFILL_MS / 1000);
  const size_t low_water_frames = (size_t)((uint64_t)SAMPLE_RATE * AUDIO_LOW_WATER_MS / 1000);

  if (!playing_) {
    if (fill_frames < prefill_frames) {
      return false;
    }
    playing_ = true;
    logf("[Audio] start (prefill=%lums fill=%u frames, buf=%u frames)",
         (unsigned long)AUDIO_PREFILL_MS,
         (unsigned)fill_frames,
         (unsigned)RING_BUF_SAMPLES);
  } else {
    if (fill_frames < low_water_frames) {
      playing_ = false;
      underruns_++;
      logf("[Audio] rebuffer (low-water=%lums fill=%u frames) underruns=%lu",
           (unsigned long)AUDIO_LOW_WATER_MS,
           (unsigned)fill_frames,
           (unsigned long)underruns_);
      return false;
    }
  }

  const size_t chunk_frames = 128;
  const size_t chunk_samples = chunk_frames * 2;
  int16_t process_buf[chunk_samples];
  size_t temp_tail = ring_tail_;
  size_t rd = 0;
  while (temp_tail != ring_head_ && rd < chunk_samples) {
    process_buf[rd++] = ring_buf_[temp_tail];
    temp_tail = (temp_tail + 1) % ring_cap_;
  }

  if (rd == 0) {
    underruns_++;
    playing_ = false;
    return false;
  }

  // If we're short, pad with zeros and count it as an underrun event.
  if (rd < chunk_samples) {
    underruns_++;
    for (size_t i = rd; i < chunk_samples; i++) process_buf[i] = 0;
    rd = chunk_samples;
  }

  const size_t frames = rd / 2;
  processSamples(process_buf, frames);
  size_t written = 0;
  // Block briefly so we don't drain the ring faster than I2S can accept data.
  // Also, only advance the ring by what was actually written.
  // IMPORTANT: this blocking write is also what throttles the audio task loop
  // to realtime (~44.1 kHz). When DMA has room it returns almost immediately;
  // when DMA is full it yields CPU to IDLE until a descriptor drains. Without
  // this, the ring empties at the CPU's whim (far faster than I2S can play).
  esp_err_t err = i2s_channel_write((i2s_chan_handle_t)i2s_tx_handle_,
                                   process_buf,
                                   rd * sizeof(int16_t),
                                   &written,
                                   pdMS_TO_TICKS(20));
  if (err != ESP_OK || written == 0) {
    // I2S backpressure without any progress: do not consume from ring, and
    // let the caller sleep a tick instead of retrying in a tight loop.
    return false;
  }
  const size_t written_samples = written / sizeof(int16_t);
  ring_tail_ = (ring_tail_ + written_samples) % ring_cap_;

  unsigned long now = nowMs();
  if (now - last_stats_ms_ >= 1000) {
    size_t fill2 = ringFillSamples() / 2;
    logf("[Audio] buf=%u/%u frames underruns=%lu overflows=%lu",
         (unsigned)fill2,
         (unsigned)RING_BUF_SAMPLES,
         (unsigned long)underruns_,
         (unsigned long)overflows_);
    last_stats_ms_ = now;
  }
  return true;
}

}  // namespace geelen
