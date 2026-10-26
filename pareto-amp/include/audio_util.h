/**
 * Audio util – portable helpers (e.g. dB to linear) for host-side tests.
 */

#ifndef GEELEN_AUDIO_UTIL_H
#define GEELEN_AUDIO_UTIL_H

#include <cstdint>

namespace geelen {

/** Convert dB gain to linear. Returns 0 for db <= -40. */
float gainDbToLinear(int8_t db);

}  // namespace geelen

#endif  // GEELEN_AUDIO_UTIL_H
