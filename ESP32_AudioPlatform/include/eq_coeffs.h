/**
 * EQ stage list builder + magnitude helpers (shared by AudioEngine and tests).
 */

#ifndef GEELEN_EQ_COEFFS_H
#define GEELEN_EQ_COEFFS_H

#include "eq_types.h"
#include <cstddef>
#include <cstdint>

namespace geelen {

constexpr size_t EQ_MAX_STAGES_PER_LAYER = 20;

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

/** Build flattened stage list from logical bands. Returns number of stages (may be 0). */
size_t eqBuildStageList(float sample_rate, const EqBand bands[EQ_MAX_BANDS], uint8_t active_band_count,
                        EqStage out[EQ_MAX_STAGES_PER_LAYER]);

/** Product of |H(f)| for all stages at frequency f_hz (linear magnitude). */
float eqLinearMagProduct(const EqStage* stages, size_t stage_count, float f_hz, float sample_rate);

}  // namespace geelen

#endif
