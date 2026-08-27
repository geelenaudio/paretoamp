// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Portable signed Q1.31 conversion used by the TAS5825M hardware PEQ path
// and by the native unit tests. Range is [-1.0, +1.0); values outside are
// saturated. NaN / Inf become 0.

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

}  // namespace geelen
