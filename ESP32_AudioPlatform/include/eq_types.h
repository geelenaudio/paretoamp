// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Shared EQ band descriptor (firmware + NVS + REST + presets).
// Packed to 16 bytes for stable NVS / on-disk layout.
//
// EQ_MAX_BANDS is the **storage** capacity of a curve — always 15 so a preset
// created on Louder (which uses all 15 hardware biquad slots) round-trips
// through Amped (which only actually applies the first 10 bands to its
// software cascade) without losing metadata. The **active** cap is
// board-specific and lives in `src/hal/hal_board.h` as
// `HAL_EQ_MAX_BANDS_ACTIVE` (10 on Amped, 15 on Louder).

#ifndef GEELEN_EQ_TYPES_H
#define GEELEN_EQ_TYPES_H

#include <cstddef>
#include <cstdint>

namespace geelen {

// Storage capacity for a single PEQ curve. Do NOT clamp `active_band_count`
// against this at API boundaries; use the HAL cap instead so a Louder-shaped
// preset can still be persisted on an Amped device.
constexpr size_t EQ_MAX_BANDS = 15;

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
