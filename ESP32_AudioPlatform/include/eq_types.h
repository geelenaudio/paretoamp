/**
 * Shared EQ band descriptor (firmware + NVS + REST + presets).
 * Packed to 16 bytes for stable NVS / on-disk layout.
 */

#ifndef GEELEN_EQ_TYPES_H
#define GEELEN_EQ_TYPES_H

#include <cstddef>
#include <cstdint>

namespace geelen {

constexpr size_t EQ_MAX_BANDS = 10;

enum class EqFilterType : uint8_t {
  Bell = 0,
  LowShelf = 1,
  HighShelf = 2,
  Notch = 3,
  LowPass = 4,
  HighPass = 5,
};

/** Corner / centre frequency Hz; gain dB; Q. `slope_db_oct` is 6, 12, or 24 for LP/HP only. */
struct EqBand {
  float freq_hz;
  float gain_db;
  float q;
  EqFilterType type;
  uint8_t slope_db_oct;  // 6, 12, or 24 for LowPass/HighPass; ignored for other types
  uint8_t _pad[2];
};

static_assert(sizeof(EqBand) == 16, "EqBand wire size");

}  // namespace geelen

#endif
