/**
 * Audio util – implementation (portable C++, no Arduino/ESP32).
 */

#include "audio_util.h"
#include <cmath>

namespace geelen {

float gainDbToLinear(int8_t db) {
  if (db <= -40) return 0.f;
  return powf(10.f, db / 20.f);
}

}  // namespace geelen
