/**
 * EQ stage expansion (RBJ biquads + first-order LP/HP) and linear magnitude product.
 */

#include "eq_coeffs.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace geelen {

namespace {

constexpr float kBypassGainDb = 0.01f;
constexpr float kDefaultShelfS = 1.f;
constexpr float kLpHpQ = 0.70710678118f;  // Butterworth 2nd order

static void normalizeBiquad(float& b0, float& b1, float& b2, float& a1, float& a2, float a0) {
  if (a0 == 0.f) return;
  b0 /= a0;
  b1 /= a0;
  b2 /= a0;
  a1 /= a0;
  a2 /= a0;
}

static bool appendBiquad(EqStage* out, size_t& idx, size_t max, float b0, float b1, float b2, float a1,
                         float a2) {
  if (idx >= max) return false;
  out[idx].kind = EqStageKind::Biquad;
  out[idx].b0 = b0;
  out[idx].b1 = b1;
  out[idx].b2 = b2;
  out[idx].a1 = a1;
  out[idx].a2 = a2;
  ++idx;
  return true;
}

static bool appendOnePoleLp(EqStage* out, size_t& idx, size_t max, float b0, float b1, float a1) {
  if (idx >= max) return false;
  out[idx].kind = EqStageKind::OnePoleLp;
  out[idx].b0 = b0;
  out[idx].b1 = b1;
  out[idx].b2 = 0.f;
  out[idx].a1 = a1;
  out[idx].a2 = 0.f;
  ++idx;
  return true;
}

static bool appendOnePoleHp(EqStage* out, size_t& idx, size_t max, float b0, float b1, float a1) {
  if (idx >= max) return false;
  out[idx].kind = EqStageKind::OnePoleHp;
  out[idx].b0 = b0;
  out[idx].b1 = b1;
  out[idx].b2 = 0.f;
  out[idx].a1 = a1;
  out[idx].a2 = 0.f;
  ++idx;
  return true;
}

static void clampFreqQ(float sr, float& freq, float& q) {
  if (freq < 20.f) freq = 20.f;
  if (freq > sr * 0.45f) freq = sr * 0.45f;
  if (q < 0.1f) q = 0.1f;
  if (q > 100.f) q = 100.f;  // allow narrow notch
}

static void coeffsBell(float sr, float freq, float gain_db, float q, float& b0, float& b1, float& b2,
                       float& a0, float& a1, float& a2) {
  const float A = powf(10.f, gain_db / 40.f);
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float alpha = sinf(w0) / (2.f * q);
  const float a0u = 1.f + alpha / A;
  b0 = 1.f + alpha * A;
  b1 = -2.f * cosw0;
  b2 = 1.f - alpha * A;
  a0 = a0u;
  a1 = -2.f * cosw0;
  a2 = 1.f - alpha / A;
  normalizeBiquad(b0, b1, b2, a1, a2, a0u);
  a0 = 1.f;
}

static void coeffsLowShelf(float sr, float freq, float gain_db, float& b0, float& b1, float& b2,
                           float& a0, float& a1, float& a2) {
  const float A = powf(10.f, gain_db / 40.f);
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float sinw0 = sinf(w0);
  const float S = kDefaultShelfS;
  const float alpha = sinw0 / 2.f * sqrtf((A + 1.f / A) * (1.f / S - 1.f) + 2.f);
  const float beta = sqrtf(A) / S;
  b0 = A * ((A + 1.f) - (A - 1.f) * cosw0 + beta * sinw0);
  b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cosw0);
  b2 = A * ((A + 1.f) - (A - 1.f) * cosw0 - beta * sinw0);
  a0 = (A + 1.f) + (A - 1.f) * cosw0 + beta * sinw0;
  a1 = -2.f * ((A - 1.f) + (A + 1.f) * cosw0);
  a2 = (A + 1.f) + (A - 1.f) * cosw0 - beta * sinw0;
  normalizeBiquad(b0, b1, b2, a1, a2, a0);
  a0 = 1.f;
}

static void coeffsHighShelf(float sr, float freq, float gain_db, float& b0, float& b1, float& b2,
                            float& a0, float& a1, float& a2) {
  const float A = powf(10.f, gain_db / 40.f);
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float sinw0 = sinf(w0);
  const float S = kDefaultShelfS;
  const float alpha = sinw0 / 2.f * sqrtf((A + 1.f / A) * (1.f / S - 1.f) + 2.f);
  const float beta = sqrtf(A) / S;
  b0 = A * ((A + 1.f) + (A - 1.f) * cosw0 + beta * sinw0);
  b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cosw0);
  b2 = A * ((A + 1.f) + (A - 1.f) * cosw0 - beta * sinw0);
  a0 = (A + 1.f) - (A - 1.f) * cosw0 + beta * sinw0;
  a1 = 2.f * ((A - 1.f) - (A + 1.f) * cosw0);
  a2 = (A + 1.f) - (A - 1.f) * cosw0 - beta * sinw0;
  normalizeBiquad(b0, b1, b2, a1, a2, a0);
  a0 = 1.f;
}

static void coeffsNotch(float sr, float freq, float q, float& b0, float& b1, float& b2, float& a0,
                        float& a1, float& a2) {
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float alpha = sinf(w0) / (2.f * q);
  b0 = 1.f;
  b1 = -2.f * cosw0;
  b2 = 1.f;
  a0 = 1.f + alpha;
  a1 = -2.f * cosw0;
  a2 = 1.f - alpha;
  normalizeBiquad(b0, b1, b2, a1, a2, a0);
  a0 = 1.f;
}

static void coeffsLp2(float sr, float freq, float q, float& b0, float& b1, float& b2, float& a0,
                      float& a1, float& a2) {
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float alpha = sinf(w0) / (2.f * q);
  b0 = (1.f - cosw0) * 0.5f;
  b1 = 1.f - cosw0;
  b2 = (1.f - cosw0) * 0.5f;
  a0 = 1.f + alpha;
  a1 = -2.f * cosw0;
  a2 = 1.f - alpha;
  normalizeBiquad(b0, b1, b2, a1, a2, a0);
  a0 = 1.f;
}

static void coeffsHp2(float sr, float freq, float q, float& b0, float& b1, float& b2, float& a0,
                      float& a1, float& a2) {
  const float w0 = 2.f * (float)M_PI * freq / sr;
  const float cosw0 = cosf(w0);
  const float alpha = sinf(w0) / (2.f * q);
  b0 = (1.f + cosw0) * 0.5f;
  b1 = -(1.f + cosw0);
  b2 = (1.f + cosw0) * 0.5f;
  a0 = 1.f + alpha;
  a1 = -2.f * cosw0;
  a2 = 1.f - alpha;
  normalizeBiquad(b0, b1, b2, a1, a2, a0);
  a0 = 1.f;
}

/** RC first-order LP: y = alpha*x + (1-alpha)*y_prev */
static void coeffsOnePoleLp(float sr, float freq, float& b0, float& b1, float& a1) {
  const float dt = 1.f / sr;
  const float RC = 1.f / (2.f * (float)M_PI * freq);
  const float alpha = dt / (RC + dt);
  b0 = alpha;
  b1 = 0.f;
  a1 = -(1.f - alpha);
}

/** RC first-order HP: y = k*(x - x_prev) + k*y_prev, k = RC/(RC+dt) */
static void coeffsOnePoleHp(float sr, float freq, float& b0, float& b1, float& a1) {
  const float dt = 1.f / sr;
  const float RC = 1.f / (2.f * (float)M_PI * freq);
  const float k = RC / (RC + dt);
  b0 = k;
  b1 = -k;
  a1 = -k;
}

static float magBiquad(float b0, float b1, float b2, float a1, float a2, float w) {
  const float cw = cosf(w);
  const float sw = sinf(w);
  const float c2w = cosf(2.f * w);
  const float s2w = sinf(2.f * w);
  const float num_re = b0 + b1 * cw + b2 * c2w;
  const float num_im = -b1 * sw - b2 * s2w;
  const float den_re = 1.f + a1 * cw + a2 * c2w;
  const float den_im = -a1 * sw - a2 * s2w;
  const float num = sqrtf(num_re * num_re + num_im * num_im);
  const float den = sqrtf(den_re * den_re + den_im * den_im);
  if (den < 1e-20f) return 1.f;
  return num / den;
}

static float magOnePole(float b0, float b1, float a1, float w) {
  const float cw = cosf(w);
  const float sw = sinf(w);
  const float num_re = b0 + b1 * cw;
  const float num_im = -b1 * sw;
  const float den_re = 1.f + a1 * cw;
  const float den_im = -a1 * sw;
  const float num = sqrtf(num_re * num_re + num_im * num_im);
  const float den = sqrtf(den_re * den_re + den_im * den_im);
  if (den < 1e-20f) return 1.f;
  return num / den;
}

}  // namespace

size_t eqBuildStageList(float sample_rate, const EqBand bands[EQ_MAX_BANDS], uint8_t active_band_count,
                        EqStage out[EQ_MAX_STAGES_PER_LAYER]) {
  const float sr = sample_rate;
  size_t idx = 0;
  if (active_band_count > EQ_MAX_BANDS) active_band_count = (uint8_t)EQ_MAX_BANDS;
  if (active_band_count < 1) active_band_count = 1;

  for (uint8_t bi = 0; bi < active_band_count; ++bi) {
    const EqBand& b = bands[bi];
    float freq = b.freq_hz;
    float gain = b.gain_db;
    float q = b.q;
    clampFreqQ(sr, freq, q);

    float b0, b1, b2, a0, a1, a2;

    switch (b.type) {
      case EqFilterType::Bell:
        if (fabsf(gain) < kBypassGainDb) continue;
        coeffsBell(sr, freq, gain, q, b0, b1, b2, a0, a1, a2);
        if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        break;

      case EqFilterType::LowShelf:
        if (fabsf(gain) < kBypassGainDb) continue;
        coeffsLowShelf(sr, freq, gain, b0, b1, b2, a0, a1, a2);
        if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        break;

      case EqFilterType::HighShelf:
        if (fabsf(gain) < kBypassGainDb) continue;
        coeffsHighShelf(sr, freq, gain, b0, b1, b2, a0, a1, a2);
        if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        break;

      case EqFilterType::Notch:
        coeffsNotch(sr, freq, q, b0, b1, b2, a0, a1, a2);
        if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        break;

      case EqFilterType::LowPass: {
        int slope = (int)b.slope_db_oct;
        if (slope != 6 && slope != 12 && slope != 24) slope = 12;
        if (slope == 6) {
          coeffsOnePoleLp(sr, freq, b0, b1, a1);
          if (!appendOnePoleLp(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, a1)) return idx;
        } else if (slope == 12) {
          coeffsLp2(sr, freq, kLpHpQ, b0, b1, b2, a0, a1, a2);
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        } else {
          coeffsLp2(sr, freq, kLpHpQ, b0, b1, b2, a0, a1, a2);
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        }
        break;
      }

      case EqFilterType::HighPass: {
        int slope = (int)b.slope_db_oct;
        if (slope != 6 && slope != 12 && slope != 24) slope = 12;
        if (slope == 6) {
          coeffsOnePoleHp(sr, freq, b0, b1, a1);
          if (!appendOnePoleHp(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, a1)) return idx;
        } else if (slope == 12) {
          coeffsHp2(sr, freq, kLpHpQ, b0, b1, b2, a0, a1, a2);
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        } else {
          coeffsHp2(sr, freq, kLpHpQ, b0, b1, b2, a0, a1, a2);
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
          if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        }
        break;
      }

      default:
        if (fabsf(gain) < kBypassGainDb) continue;
        coeffsBell(sr, freq, gain, q, b0, b1, b2, a0, a1, a2);
        if (!appendBiquad(out, idx, EQ_MAX_STAGES_PER_LAYER, b0, b1, b2, a1, a2)) return idx;
        break;
    }
  }
  return idx;
}

float eqLinearMagProduct(const EqStage* stages, size_t stage_count, float f_hz, float sample_rate) {
  const float w = 2.f * (float)M_PI * f_hz / sample_rate;
  float prod = 1.f;
  for (size_t i = 0; i < stage_count; ++i) {
    const EqStage& s = stages[i];
    float m = 1.f;
    switch (s.kind) {
      case EqStageKind::Biquad:
        m = magBiquad(s.b0, s.b1, s.b2, s.a1, s.a2, w);
        break;
      case EqStageKind::OnePoleLp:
      case EqStageKind::OnePoleHp:
        m = magOnePole(s.b0, s.b1, s.a1, w);
        break;
      default:
        break;
    }
    prod *= m;
  }
  return prod;
}

}  // namespace geelen
