// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Portable Q-format helpers: Q1.31 (legacy / tests) and TAS58xx Q5.27
// (hardware PEQ). Also amplitude-style percent → TAS digital volume 0x4C.

#pragma once

#include <cstdint>
#include <cmath>

namespace geelen {

inline int32_t eqFloatToQ1_31(float v) {
  if (std::isnan(v) || std::isinf(v)) return 0;
  if (v <= -1.0f) return INT32_MIN;
  if (v >= 1.0f) return INT32_MAX;
  double x = (double)v * 2147483648.0;
  if (x >= 2147483647.0) return INT32_MAX;
  if (x <= -2147483648.0) return INT32_MIN;
  return (int32_t)(x >= 0 ? x + 0.5 : x - 0.5);
}

inline float eqQ1_31ToFloat(int32_t q) {
  return (float)((double)q / 2147483648.0);
}

// TAS5825M DSP biquads are Q5.27 (1.0 = 0x08000000), range [-16, +16).
inline int32_t eqFloatToQ5_27(float v) {
  if (std::isnan(v) || std::isinf(v)) return 0;
  if (v >= 16.0f) return INT32_MAX;
  if (v <= -16.0f) return INT32_MIN;
  double x = (double)v * 134217728.0;  // 2^27
  if (x >= 2147483647.0) return INT32_MAX;
  if (x <= -2147483648.0) return INT32_MIN;
  return (int32_t)(x >= 0 ? x + 0.5 : x - 0.5);
}

// Pack one EqStage into the TAS5825M 20-byte BQ slot (b0,b1,b2,a1,a2).
//
// Software IIR (and RBJ textbook form) is:
//   y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2
// TAS58xx DSP (TI Direct Form, SLOA263 / PPC3 dumps) is:
//   y = b0*x + b1*x1 + b2*x2 + a1*y1 + a2*y2
// so hardware a1/a2 are the negated textbook values. Writing the software
// signs unchanged puts the poles outside the unit circle and the DSP
// output goes to zero until EQ is bypassed.
inline void eqStageToTas5825mQ5_27(float b0, float b1, float b2, float a1, float a2,
                                   int32_t out[5]) {
  out[0] = eqFloatToQ5_27(b0);
  out[1] = eqFloatToQ5_27(b1);
  out[2] = eqFloatToQ5_27(b2);
  out[3] = eqFloatToQ5_27(-a1);
  out[4] = eqFloatToQ5_27(-a2);
}

// TAS58xx digital volume register 0x4C (keep in sync with tas5805m_set_volume_pct):
//   0x00 = +24 dB, 0x30 = 0 dB, 0.5 dB/step down to 0xFE = −103 dB, 0xFF = mute.
// User fader never boosts above 0 dB. Map percent like software amplitude:
//   dB = 20*log10(pct/100), mute at 0. 31% is about −10 dB (reg 0x44), not −69 dB.
inline uint8_t eqVolumePctToTasDigVol(uint8_t pct) {
  if (pct == 0) return 0xFF;
  if (pct >= 100) return 0x30;
  const double db = 20.0 * std::log10(static_cast<double>(pct) / 100.0);
  long steps = std::lround(-db / 0.5);
  if (steps < 0) steps = 0;
  long val = 0x30L + steps;
  if (val > 0xFE) val = 0xFE;
  return static_cast<uint8_t>(val);
}

}  // namespace geelen
