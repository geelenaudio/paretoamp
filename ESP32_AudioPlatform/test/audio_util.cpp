/**
 * Audio util – compiled with native test (same as src/audio_util.cpp).
 */

#include "audio_util.h"
#include <cmath>

namespace geelen {

float gainDbToLinear(int8_t db) {
  if (db <= -40) return 0.f;
  return powf(10.f, db / 20.f);
}

}  // namespace geelen
