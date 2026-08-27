// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// EQ stage list builder + magnitude helpers (shared by AudioEngine and tests).

#ifndef GEELEN_EQ_COEFFS_H
#define GEELEN_EQ_COEFFS_H

#include "eq_types.h"
#include <cstddef>
#include <cstdint>

namespace geelen {

// Upper bound on the stage count from expanding a 15-band PEQ. Every band
// contributes 0..2 stages (24 dB/oct LP/HP is 2 biquads); 15 bands × 2 = 30.
// The AudioEngine allocates buffers of this size, so raising it in one place
// keeps everything else automatically sized.
constexpr size_t EQ_MAX_STAGES_PER_LAYER = 30;

enum class EqStageKind : uint8_t {
  Biquad = 0,
  OnePoleLp = 1,
  OnePoleHp = 2,
};

struct EqStage {
  EqStageKind kind;
  uint8_t _pad[3];
  float b0, b1, b2;
  float a1, a2;  // a0 normalized to 1
};

/**
 * Build flattened stage list from logical bands. Reads up to
 * `active_band_count` bands from `bands[]` and writes up to
 * `EQ_MAX_STAGES_PER_LAYER` biquad/first-order stages into `out[]`.
 * Returns the number of stages actually written (may be 0 if every band
 * has near-zero gain and is optimised out).
 */
size_t eqBuildStageList(float sample_rate, const EqBand* bands, uint8_t active_band_count,
                        EqStage out[EQ_MAX_STAGES_PER_LAYER]);

/** Product of |H(f)| for all stages at frequency f_hz (linear magnitude). */
float eqLinearMagProduct(const EqStage* stages, size_t stage_count, float f_hz, float sample_rate);

}  // namespace geelen

#endif
