// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Audio Engine — single-layer parametric EQ, inter-sample peak limiter,
// PSRAM ring, I2S out.

#include "audio_engine.h"
#include "audio_util.h"
#include "hal/hal_board.h"
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <cstdarg>
#include <cstdio>
#ifndef ARDUINO
#include <esp_attr.h>
#include <esp_log.h>
#else
#define IRAM_ATTR
#endif
#include <cmath>
#include <cstring>

#include "eq_coeffs.h"

namespace geelen {

namespace {

/** 128 frames @ 44.1 kHz ≈ 2902 µs; log if processing exceeds 70% of that. */
constexpr uint64_t kProcessBudgetUs = (uint64_t)((128.0 * 1e6 / 44100.0) * 0.7);

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
  ESP_LOGI("Audio", "%s", buf);
#endif
  va_end(ap);
}

// Legacy 10-band graphic EQ centres, retained only for `setEQGains`
// (the Snapcast `ServerSettings` graphic-EQ compat path). Everything else
// uses the caller-provided `EqBand.freq_hz`.
static constexpr size_t LEGACY_GRAPHIC_EQ_BANDS = 10;
static const float LEGACY_GRAPHIC_EQ_CENTER_HZ[LEGACY_GRAPHIC_EQ_BANDS] = {
  31.f, 62.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f
};
static constexpr float DEFAULT_Q = 1.2f;

static EqBand makeDefaultBand(size_t i) {
  EqBand b{};
  // First 10 bands seed the legacy centres; remaining 5 spread logarithmically
  // between 20 kHz and 22 kHz so they render sanely if the SPA ever surfaces
  // them on an Amped device.
  if (i < LEGACY_GRAPHIC_EQ_BANDS) {
    b.freq_hz = LEGACY_GRAPHIC_EQ_CENTER_HZ[i];
  } else {
    b.freq_hz = 16000.f + 1000.f * (float)(i - LEGACY_GRAPHIC_EQ_BANDS + 1);
  }
  b.gain_db = 0.f;
  b.q = DEFAULT_Q;
  b.type = EqFilterType::Bell;
  b.slope_db_oct = 12;
  b._pad[0] = b._pad[1] = 0;
  return b;
}

}  // namespace

void AudioEngine::initBiquads() {
  cascade_.enabled = false;
  cascade_.active_band_count = (uint8_t)AUDIO_EQ_BANDS;
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) {
    cascade_.bands[i] = makeDefaultBand(i);
  }
  for (size_t s = 0; s < EQ_MAX_STAGES_PER_LAYER; ++s) {
    for (int ch = 0; ch < 2; ++ch) {
      cascade_.zx1[s][ch] = cascade_.zx2[s][ch] = 0.f;
      cascade_.zy1[s][ch] = cascade_.zy2[s][ch] = 0.f;
    }
  }
  rebuildCascade();
  headroom_linear_ = 1.f;
  intersample_limiter_gain_ = 1.f;
  volume_ = 1.f;
  volume_l_ = 1.f;
  volume_r_ = 1.f;
  muted_ = false;
  l_on_ = true;
  r_on_ = true;
  mono_ = false;
}

void AudioEngine::rebuildCascade() {
  Cascade& c = cascade_;
  if (c.active_band_count < 1) c.active_band_count = 1;
  const uint8_t hw_cap = maxBands();
  if (c.active_band_count > hw_cap) c.active_band_count = hw_cap;
  c.stage_count = (uint8_t)eqBuildStageList((float)SAMPLE_RATE, c.bands, c.active_band_count, c.stages);
  for (size_t s = 0; s < EQ_MAX_STAGES_PER_LAYER; ++s) {
    for (int ch = 0; ch < 2; ++ch) {
      c.zx1[s][ch] = c.zx2[s][ch] = 0.f;
      c.zy1[s][ch] = c.zy2[s][ch] = 0.f;
    }
  }
}

uint8_t AudioEngine::maxBands() const {
  return (uint8_t)HAL_EQ_MAX_BANDS_ACTIVE;
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
    i2s_tx_errors_(0),
    i2s_blocked_us_total_(0),
    last_stats_blocked_us_(0),
    last_stats_ms_(0),
    last_contention_log_us_(0),
    last_i2s_backpressure_log_us_(0),
    intersample_limiter_gain_(1.f) {
  initBiquads();
}

size_t AudioEngine::ringFillSamplesLocked() const {
  size_t h = ring_head_.load(std::memory_order_acquire);
  size_t t = ring_tail_.load(std::memory_order_relaxed);
  if (h >= t) return h - t;
  return ring_cap_ - (t - h);
}

size_t AudioEngine::ringFillSamples() const {
  return ringFillSamplesLocked();
}

size_t AudioEngine::bufferedFrames() const { return ringFillSamples() / 2; }

void AudioEngine::setI2SChannel(void* tx_handle) { i2s_tx_handle_ = tx_handle; }

AudioEngine::~AudioEngine() {
  if (eq_mutex_) {
    vSemaphoreDelete((SemaphoreHandle_t)eq_mutex_);
    eq_mutex_ = nullptr;
  }
  if (producer_mutex_) {
    vSemaphoreDelete((SemaphoreHandle_t)producer_mutex_);
    producer_mutex_ = nullptr;
  }
  if (ring_buf_) {
    heap_caps_free(ring_buf_);
    ring_buf_ = nullptr;
  }
}

bool AudioEngine::begin() {
  if (!eq_mutex_) {
    eq_mutex_ = xSemaphoreCreateMutex();
    if (!eq_mutex_) return false;
  }
  if (!producer_mutex_) {
    producer_mutex_ = xSemaphoreCreateMutex();
    if (!producer_mutex_) return false;
  }
  ring_cap_ = RING_BUF_SAMPLES * 2;
  ring_buf_ = (int16_t*)heap_caps_malloc(RING_BUF_BYTES, MALLOC_CAP_SPIRAM);
  if (!ring_buf_) ring_buf_ = (int16_t*)malloc(RING_BUF_BYTES);
  if (!ring_buf_) return false;
  ring_head_.store(0, std::memory_order_relaxed);
  ring_tail_.store(0, std::memory_order_relaxed);
  playing_ = false;
  underruns_ = 0;
  overflows_ = 0;
  last_stats_ms_ = nowMs();
  running_ = true;
  return true;
}

void AudioEngine::setEQGains(const int8_t gains[AUDIO_EQ_BANDS]) {
  // Legacy graphic-EQ path (Snapcast ServerSettings). Maps the first 10 bands
  // to fixed centres and pads any trailing bands with 0 dB / DEFAULT_Q.
  SemaphoreHandle_t em = eq_mutex_ ? (SemaphoreHandle_t)eq_mutex_ : nullptr;
  if (em) xSemaphoreTake(em, portMAX_DELAY);
  Cascade& c = cascade_;
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) {
    c.bands[i].freq_hz = (i < LEGACY_GRAPHIC_EQ_BANDS)
                             ? LEGACY_GRAPHIC_EQ_CENTER_HZ[i]
                             : 16000.f + 1000.f * (float)(i - LEGACY_GRAPHIC_EQ_BANDS + 1);
    c.bands[i].gain_db = (i < LEGACY_GRAPHIC_EQ_BANDS) ? (float)gains[i] : 0.f;
    c.bands[i].q = DEFAULT_Q;
    c.bands[i].type = EqFilterType::Bell;
    c.bands[i].slope_db_oct = 12;
  }
  c.active_band_count = (uint8_t)LEGACY_GRAPHIC_EQ_BANDS;
  c.enabled = true;
  rebuildCascade();
  eq_peak_db_ = computeEqPeakGainDbUnlocked();
  eq_peak_valid_ = true;
  if (em) xSemaphoreGive(em);
}

void AudioEngine::setEqCurve(const EqBand* bands, uint8_t active_band_count, bool enabled) {
  if (!bands) return;
  SemaphoreHandle_t em = eq_mutex_ ? (SemaphoreHandle_t)eq_mutex_ : nullptr;
  if (em) xSemaphoreTake(em, portMAX_DELAY);
  Cascade& c = cascade_;
  for (size_t i = 0; i < AUDIO_EQ_BANDS; ++i) c.bands[i] = bands[i];
  if (active_band_count < 1) active_band_count = 1;
  if (active_band_count > (uint8_t)AUDIO_EQ_BANDS) active_band_count = (uint8_t)AUDIO_EQ_BANDS;
  const uint8_t hw_cap = maxBands();
  if (active_band_count > hw_cap) active_band_count = hw_cap;
  c.active_band_count = active_band_count;
  c.enabled = enabled;
  rebuildCascade();
  eq_peak_db_ = computeEqPeakGainDbUnlocked();
  eq_peak_valid_ = true;
  if (em) xSemaphoreGive(em);
}

void AudioEngine::setEqBackend(EqBackend b) {
  eq_backend_ = b;
}

void AudioEngine::setHeadroomDb(float db) {
  if (db > 0.f) db = 0.f;
  if (db < -60.f) db = -60.f;
  headroom_linear_ = powf(10.f, db / 20.f);
}

float AudioEngine::computeEqPeakGainDbUnlocked() const {
  constexpr int N = 384;
  const float sr = (float)SAMPLE_RATE;
  const float f_lo = 20.f;
  const float f_hi = 20000.f;
  const float log_lo = std::log(f_lo);
  const float log_hi = std::log(f_hi);

  float peak = 1e-12f;
  const Cascade& c = cascade_;
  const bool active = c.enabled && c.stage_count > 0;
  for (int i = 0; i < N; ++i) {
    const float f = expf(log_lo + (log_hi - log_lo) * (float)i / (float)(N - 1));
    float mag = active ? eqLinearMagProduct(c.stages, c.stage_count, f, sr) : 1.f;
    if (mag > peak) peak = mag;
  }
  return 20.f * log10f(peak) + EQ_INTERSAMPLE_HEADROOM_DB;
}

float AudioEngine::computeEqPeakGainDb() const {
  if (!eq_mutex_) return computeEqPeakGainDbUnlocked();
  SemaphoreHandle_t em = (SemaphoreHandle_t)eq_mutex_;
  xSemaphoreTake(em, portMAX_DELAY);
  if (!eq_peak_valid_) {
    const_cast<AudioEngine*>(this)->eq_peak_db_ = computeEqPeakGainDbUnlocked();
    const_cast<AudioEngine*>(this)->eq_peak_valid_ = true;
  }
  const float r = eq_peak_db_;
  xSemaphoreGive(em);
  return r;
}

void AudioEngine::setVolume(uint8_t vol_percent) {
  if (vol_percent > 100) vol_percent = 100;
  volume_ = vol_percent / 100.f;
  volume_l_ = volume_r_ = volume_;
}

void AudioEngine::setVolumeLR(uint8_t l_pct, uint8_t r_pct) {
  if (l_pct > 100) l_pct = 100;
  if (r_pct > 100) r_pct = 100;
  volume_l_ = l_pct / 100.f;
  volume_r_ = r_pct / 100.f;
  volume_ = volume_l_ > volume_r_ ? volume_l_ : volume_r_;
}

void AudioEngine::setMuted(bool muted) { muted_ = muted; }

void AudioEngine::setChannels(bool l_on, bool r_on, bool mono) {
  l_on_ = l_on;
  r_on_ = r_on;
  mono_ = mono;
}

void AudioEngine::feedPCM(const uint8_t* data, size_t len_bytes) {
  if (!ring_buf_ || !data) return;
  if (producer_mutex_) xSemaphoreTake((SemaphoreHandle_t)producer_mutex_, portMAX_DELAY);
  size_t num_samples = len_bytes / sizeof(int16_t);
  const int16_t* src = (const int16_t*)data;
  size_t head = ring_head_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < num_samples; i++) {
    size_t next = (head + 1) % ring_cap_;
    const size_t tail = ring_tail_.load(std::memory_order_acquire);
    if (next == tail) {
      overflows_++;
      break;
    }
    ring_buf_[head] = src[i];
    head = next;
  }
  ring_head_.store(head, std::memory_order_release);
  if (producer_mutex_) xSemaphoreGive((SemaphoreHandle_t)producer_mutex_);
}

IRAM_ATTR void AudioEngine::processSamples(int16_t* buf, size_t num_frames) {
  const bool mono = mono_;
  const bool muted = muted_;
  const bool lon = l_on_;
  const bool ron = r_on_;
  const float gL = muted ? 0.f : volume_l_;
  const float gR = muted ? 0.f : volume_r_;
  const float hr = headroom_linear_;

  if (num_frames > 128) num_frames = 128;
  float work[128 * 2];

  Cascade& c = cascade_;
  const bool eq_active = c.enabled && c.stage_count > 0;
  // Ganged stereo DF1: same b0..a2 for L and R, independent delay state.
  // A split Amped curve would still be N_L + N_R evals (not 2× this loop)
  // but extra DRAM coeff loads on a path that already reports dma_wait
  // ~12–20% on Amped-WROVER. Keep one stage list; see docs/eq.md.
  for (size_t f = 0; f < num_frames; f++) {
    float L = (float)buf[f * 2];
    float R = (float)buf[f * 2 + 1];
    if (eq_active) {
      for (uint8_t si = 0; si < c.stage_count; ++si) {
        const EqStage& st = c.stages[si];
        const float x0L = L;
        const float x0R = R;
        float y0L, y0R;
        if (st.kind == EqStageKind::Biquad) {
          y0L = st.b0 * x0L + st.b1 * c.zx1[si][0] + st.b2 * c.zx2[si][0] - st.a1 * c.zy1[si][0] -
                st.a2 * c.zy2[si][0];
          y0R = st.b0 * x0R + st.b1 * c.zx1[si][1] + st.b2 * c.zx2[si][1] - st.a1 * c.zy1[si][1] -
                st.a2 * c.zy2[si][1];
          c.zx2[si][0] = c.zx1[si][0];
          c.zx1[si][0] = x0L;
          c.zy2[si][0] = c.zy1[si][0];
          c.zy1[si][0] = y0L;
          c.zx2[si][1] = c.zx1[si][1];
          c.zx1[si][1] = x0R;
          c.zy2[si][1] = c.zy1[si][1];
          c.zy1[si][1] = y0R;
        } else {
          y0L = st.b0 * x0L + st.b1 * c.zx1[si][0] - st.a1 * c.zy1[si][0];
          y0R = st.b0 * x0R + st.b1 * c.zx1[si][1] - st.a1 * c.zy1[si][1];
          c.zx1[si][0] = x0L;
          c.zy1[si][0] = y0L;
          c.zx1[si][1] = x0R;
          c.zy1[si][1] = y0R;
          c.zx2[si][0] = c.zx2[si][1] = 0.f;
          c.zy2[si][0] = c.zy2[si][1] = 0.f;
        }
        L = y0L;
        R = y0R;
      }
    }
    L *= hr;
    R *= hr;
    work[f * 2] = L;
    work[f * 2 + 1] = R;
  }

  // Short-circuit the true-peak limiter when the cascade is bypassed and the
  // EQ contributes no positive gain. In that case `work[] = buf[] * hr` with
  // `hr <= 1`, input was int16 so |work[]| <= 32767, and the linear midpoint
  // can only be smaller — so the pk scan is guaranteed to produce target=1.f.
  // Skips ~510 abs+compares per 128-frame chunk (~25 % of processSamples in the
  // trivial pass-through case).
  const bool skip_pk_scan = !eq_active && eq_peak_db_ <= 0.f;

  if (!skip_pk_scan) {
    float pk = 0.f;
    for (size_t f = 0; f < num_frames; ++f) {
      float a = fabsf(work[f * 2]);
      if (a > pk) pk = a;
      a = fabsf(work[f * 2 + 1]);
      if (a > pk) pk = a;
    }
    for (size_t f = 0; f + 1 < num_frames; ++f) {
      float mL = 0.5f * (work[f * 2] + work[(f + 1) * 2]);
      float mR = 0.5f * (work[f * 2 + 1] + work[(f + 1) * 2 + 1]);
      float a = fabsf(mL);
      if (a > pk) pk = a;
      a = fabsf(mR);
      if (a > pk) pk = a;
    }
    // True-peak style check uses linear midpoints; a naive single scale per 128-sample
    // chunk causes loudness steps at chunk boundaries (heard as crackle/zipper) when
    // peaks only appear on some chunks — common with HP/LP overshoot. Smooth gain.
    const float target_scale = (pk > 32767.f) ? (32767.f / pk) : 1.f;
    if (target_scale < intersample_limiter_gain_) {
      intersample_limiter_gain_ = target_scale;
    } else {
      constexpr float kReleasePerChunk = 0.08f;
      intersample_limiter_gain_ +=
          (1.f - intersample_limiter_gain_) * kReleasePerChunk;
      if (intersample_limiter_gain_ > 0.9995f) intersample_limiter_gain_ = 1.f;
    }
  } else if (intersample_limiter_gain_ < 1.f) {
    // Release the limiter even when we're skipping the scan, so a previous
    // EQ-driven attack doesn't get stuck below unity once EQ is bypassed.
    constexpr float kReleasePerChunk = 0.08f;
    intersample_limiter_gain_ +=
        (1.f - intersample_limiter_gain_) * kReleasePerChunk;
    if (intersample_limiter_gain_ > 0.9995f) intersample_limiter_gain_ = 1.f;
  }
  const float scale = intersample_limiter_gain_;

  for (size_t f = 0; f < num_frames; f++) {
    float L = work[f * 2] * scale;
    float R = work[f * 2 + 1] * scale;
    if (mono) {
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
    buf[f * 2] = (int16_t)outL;
    buf[f * 2 + 1] = (int16_t)outR;
  }
}

bool AudioEngine::process() {
  if (!ring_buf_ || !running_) return false;
  if (!i2s_tx_handle_) return false;

  const size_t chunk_frames = 128;
  const size_t chunk_samples = chunk_frames * 2;
  int16_t process_buf[chunk_samples];
  size_t rd = 0;

  {
    const size_t fill_samples = ringFillSamplesLocked();
    const size_t fill_frames = fill_samples / 2;
    const size_t prefill_frames = (size_t)((uint64_t)SAMPLE_RATE * AUDIO_PREFILL_MS / 1000);
    const size_t low_water_frames = (size_t)((uint64_t)SAMPLE_RATE * AUDIO_LOW_WATER_MS / 1000);

    if (!playing_) {
      if (fill_frames < prefill_frames) {
#if VERBOSE_AUDIO_LOG
        // Rate-limited so we get one line per second even when the audio task
        // is polling at ~100 Hz. This is the smoking-gun log for "voice prompt
        // never started" — if the ring sits below prefill while a prompt is
        // playing, the producer side is the bug.
        static unsigned long s_last_prefill_log_ms = 0;
        const unsigned long now_ms = nowMs();
        if (now_ms - s_last_prefill_log_ms > 1000) {
          s_last_prefill_log_ms = now_ms;
          logf("[Audio] waiting for prefill: fill=%u/%u frames",
               (unsigned)fill_frames, (unsigned)prefill_frames);
        }
#endif
        return false;
      }
      playing_ = true;
      logf("[Audio] start (prefill=%lums fill=%u frames, buf=%u frames)",
           (unsigned long)AUDIO_PREFILL_MS, (unsigned)fill_frames, (unsigned)RING_BUF_SAMPLES);
    } else {
      if (fill_frames < low_water_frames) {
        playing_ = false;
        underruns_++;
        logf("[Audio] rebuffer (low-water=%lums fill=%u frames) underruns=%lu",
             (unsigned long)AUDIO_LOW_WATER_MS, (unsigned)fill_frames, (unsigned long)underruns_);
        return false;
      }
    }

    size_t temp_tail = ring_tail_.load(std::memory_order_relaxed);
    const size_t head = ring_head_.load(std::memory_order_acquire);
    while (temp_tail != head && rd < chunk_samples) {
      process_buf[rd++] = ring_buf_[temp_tail];
      temp_tail = (temp_tail + 1) % ring_cap_;
    }

    if (rd == 0) {
      underruns_++;
      playing_ = false;
      return false;
    }

    if (rd < chunk_samples) {
      underruns_++;
      for (size_t i = rd; i < chunk_samples; i++) process_buf[i] = 0;
      rd = chunk_samples;
    }
  }

  const size_t frames = rd / 2;

  if (eq_mutex_) xSemaphoreTake((SemaphoreHandle_t)eq_mutex_, portMAX_DELAY);

#ifndef ARDUINO
  const uint64_t t0 = esp_timer_get_time();
#endif
  processSamples(process_buf, frames);
  if (eq_mutex_) xSemaphoreGive((SemaphoreHandle_t)eq_mutex_);
  updateOutputMeters(process_buf, frames);
#ifndef ARDUINO
  const uint64_t t1 = esp_timer_get_time();
  const uint64_t dt = t1 - t0;
  if (dt > kProcessBudgetUs) {
    if (t1 - last_contention_log_us_ > 1000000ULL) {
      last_contention_log_us_ = t1;
      const Cascade& c = cascade_;
      ESP_LOGW("Audio",
               "EQ processing slow: %llu us (budget %llu) en=%d stages=%u active_bands=%u",
               (unsigned long long)dt, (unsigned long long)kProcessBudgetUs, (int)c.enabled,
               (unsigned)c.stage_count, (unsigned)c.active_band_count);
    }
  }
#endif

  // I2S write path: one blocking write per chunk (paced by DMA). No retry/rollback —
  // `processSamples` has already committed its IIR state and the bytes are headed for DMA.
  // We only treat a write as a real "stall" if the driver returns a hard `ESP_ERR_*`
  // *and* the cumulative wait for this chunk has exceeded ~2 s (genuine driver hang).
#ifndef ARDUINO
  uint8_t* out_ptr = reinterpret_cast<uint8_t*>(process_buf);
  size_t remain = rd * sizeof(int16_t);
  const uint64_t write_start_us = esp_timer_get_time();
  constexpr uint64_t kHardStallUs = 2000000ULL;  // 2 seconds = real driver hang

  while (remain > 0) {
    size_t w = 0;
    const uint64_t bw0 = esp_timer_get_time();
    esp_err_t err = i2s_channel_write((i2s_chan_handle_t)i2s_tx_handle_, out_ptr, remain, &w,
                                      portMAX_DELAY);
    const uint64_t bw1 = esp_timer_get_time();
    i2s_blocked_us_total_ += (bw1 - bw0);
    if (err == ESP_OK && w > 0) {
      out_ptr += w;
      remain -= w;
      continue;
    }
    // Real driver error (or zero bytes despite portMAX_DELAY). Only act on a true hang.
    const uint64_t waited = bw1 - write_start_us;
    if (waited >= kHardStallUs) {
      i2s_tx_errors_++;
      if (bw1 - last_i2s_backpressure_log_us_ > 10000000ULL) {
        last_i2s_backpressure_log_us_ = bw1;
        ESP_LOGW("Audio", "I2S TX hard stall: err=%d waited=%llu us; flushing ring + resetting IIR",
                 (int)err, (unsigned long long)waited);
      }
      {
        size_t t = ring_tail_.load(std::memory_order_relaxed);
        ring_tail_.store((t + rd) % ring_cap_, std::memory_order_release);
      }
      if (eq_mutex_) xSemaphoreTake((SemaphoreHandle_t)eq_mutex_, portMAX_DELAY);
      rebuildCascade();
      eq_peak_db_ = computeEqPeakGainDbUnlocked();
      eq_peak_valid_ = true;
      if (eq_mutex_) xSemaphoreGive((SemaphoreHandle_t)eq_mutex_);
      return true;
    }
    // Soft transient (driver returned without writing but we haven't waited long).
    // Never tight-spin here: some builds can return immediately with w==0 and/or a
    // non-OK err without sleeping the full portMAX_DELAY budget. A spin pegs Core 1,
    // starves the MP3/A2DP producers and the BT controller (symptom: BT_APPL Pkt dropped,
    // silence — no `[Audio]` follow-up if we never finish the first chunk).
    if (err != ESP_OK) {
      i2s_tx_errors_++;
      if (bw1 - last_i2s_backpressure_log_us_ > 10000000ULL) {
        last_i2s_backpressure_log_us_ = bw1;
        ESP_LOGW("Audio", "I2S TX transient err=%d (waited=%llu us, retrying)", (int)err,
                 (unsigned long long)waited);
      }
    } else if (w == 0) {
      if (bw1 - last_i2s_backpressure_log_us_ > 10000000ULL) {
        last_i2s_backpressure_log_us_ = bw1;
        ESP_LOGW("Audio", "I2S TX returned OK with written=0 (waited=%llu us, retrying)",
                 (unsigned long long)waited);
      }
    }
    vTaskDelay(1);
  }
#else
  size_t written = 0;
  const uint64_t bw0 = esp_timer_get_time();
  esp_err_t err = i2s_channel_write((i2s_chan_handle_t)i2s_tx_handle_, process_buf, rd * sizeof(int16_t),
                                    &written, portMAX_DELAY);
  i2s_blocked_us_total_ += (esp_timer_get_time() - bw0);
  if (err != ESP_OK) {
    i2s_tx_errors_++;
    return false;
  }
#endif

  {
    size_t t = ring_tail_.load(std::memory_order_relaxed);
    ring_tail_.store((t + rd) % ring_cap_, std::memory_order_release);
  }

  unsigned long now = nowMs();
  if (now - last_stats_ms_ >= 1000) {
    size_t fill2 = ringFillSamples() / 2;
    const uint64_t blocked_now = i2s_blocked_us_total_;
    const uint64_t blocked_delta = blocked_now - last_stats_blocked_us_;
    const unsigned long elapsed_ms = now - last_stats_ms_;
    // dma_wait=% ≈ fraction of wall time the audio task spent inside i2s_channel_write.
    // With portMAX_DELAY that is almost all "blocked until DMA has room" — i.e. the
    // hardware clock pacing realtime output. High values (~60–80%) are normal and
    // mean the task is *not* busy-spinning between chunks. Math: blocked_us/(elapsed_ms*10).
    // Amped software 10-band PEQ often reports ~12–20% — that is compute-bound,
    // not healthy slack. Do not add a second software curve (docs/eq.md).
    const unsigned blocked_pct =
      elapsed_ms > 0 ? (unsigned)(blocked_delta / (10ULL * (uint64_t)elapsed_ms)) : 0;
    last_stats_blocked_us_ = blocked_now;
    logf("[Audio] buf=%u/%u underruns=%lu overflows=%lu i2s_err=%lu dma_wait=%u%%",
         (unsigned)fill2, (unsigned)RING_BUF_SAMPLES, (unsigned long)underruns_,
         (unsigned long)overflows_, (unsigned long)i2s_tx_errors_, blocked_pct);
    last_stats_ms_ = now;
  }
  return true;
}

void AudioEngine::updateOutputMeters(const int16_t* buf, size_t num_frames) {
  if (!buf || num_frames == 0) return;
  float cPkL = 0.f, cPkR = 0.f;
  double sL = 0.0, sR = 0.0;
  for (size_t f = 0; f < num_frames; ++f) {
    const float x = (float)buf[f * 2] * (1.f / 32767.f);
    const float y = (float)buf[f * 2 + 1] * (1.f / 32767.f);
    const float ax = fabsf(x);
    const float ay = fabsf(y);
    if (ax > cPkL) cPkL = ax;
    if (ay > cPkR) cPkR = ay;
    sL += (double)x * (double)x;
    sR += (double)y * (double)y;
  }
  const float invn = 1.f / (float)num_frames;
  const float msL = (float)(sL * (double)invn);
  const float msR = (float)(sR * (double)invn);
  constexpr float kEma = 0.08f;
  const float oldL = meter_rms_ms_l_.load(std::memory_order_relaxed);
  const float oldR = meter_rms_ms_r_.load(std::memory_order_relaxed);
  meter_rms_ms_l_.store(oldL * (1.f - kEma) + msL * kEma, std::memory_order_relaxed);
  meter_rms_ms_r_.store(oldR * (1.f - kEma) + msR * kEma, std::memory_order_relaxed);
  constexpr float kPkDecay = 0.95f;
  float pL = meter_peak_lin_l_.load(std::memory_order_relaxed);
  float pR = meter_peak_lin_r_.load(std::memory_order_relaxed);
  pL = fmaxf(cPkL, pL * kPkDecay);
  pR = fmaxf(cPkR, pR * kPkDecay);
  meter_peak_lin_l_.store(pL, std::memory_order_relaxed);
  meter_peak_lin_r_.store(pR, std::memory_order_relaxed);
}

void AudioEngine::readOutputMeters(float& peak_l_db, float& peak_r_db, float& rms_l_db, float& rms_r_db) const {
  auto toDb = [](float lin) {
    lin = fmaxf(lin, 1e-8f);
    return 20.f * log10f(lin);
  };
  const float pl = meter_peak_lin_l_.load(std::memory_order_relaxed);
  const float pr = meter_peak_lin_r_.load(std::memory_order_relaxed);
  const float rl = sqrtf(fmaxf(meter_rms_ms_l_.load(std::memory_order_relaxed), 0.f));
  const float rr = sqrtf(fmaxf(meter_rms_ms_r_.load(std::memory_order_relaxed), 0.f));
  peak_l_db = toDb(pl);
  peak_r_db = toDb(pr);
  rms_l_db = toDb(rl);
  rms_r_db = toDb(rr);
}

}  // namespace geelen
