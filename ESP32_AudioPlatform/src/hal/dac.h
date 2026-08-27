// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Hardware Abstraction Layer for the DAC that sits between our I2S output
// and the amplifier stage. Two implementations exist today:
//
//   - dac_none.cpp     — used on all Amped boards. The PCM5100A has no
//                        register-programmable controls, so `dacInit()` is
//                        a no-op and `dacApplyPeqBands()` always returns
//                        `false` (the caller keeps the software PEQ path).
//   - dac_tas5825m.cpp — used on Louder boards. Wraps the vendored sonocotta
//                        driver at `components/tas5805m/` and implements
//                        hardware PEQ by uploading Q1.31 biquad coefficients
//                        into the TAS5825M's 15 native BQ slots per channel.
//
// The right implementation is picked at build time by the src `build_src_filter`
// in `platformio.ini`. Callers never need to know which one they got — the
// `dacSupportsHwPeq()` boolean tells them whether to use the HW backend or
// stay on software cascades.

#pragma once

#include <cstddef>
#include <cstdint>

#include "eq_types.h"
#include "hal/hal_board.h"

namespace geelen_hal {

// Bring the DAC up to a known state. On Amped this is trivial (there is no
// device to configure). On Louder this initializes the I2C bus, verifies the
// TAS5825M is on-address, and drives it to the HI-Z state so it stays silent
// while I2S clocks stabilise. Returns true on success; on failure the caller
// should still flip the amp enable pin so audio at least reaches the analog
// stage (a Louder board will just stay muted until reboot).
bool dacInit();

// Bring the amp stage from HI-Z to PLAY (or the reverse). On Amped this is
// just `gpio_set_level(HAL_PIN_AMP_EN, on)`. On Louder this runs the
// TAS5825M state machine transitions (HI-Z → DEEP_SLEEP → SLEEP → HI-Z →
// PLAY per datasheet Table 6-3). Both variants keep the historical Amped
// contract: caller waits `HAL_I2S_STABLE_MS` after `i2s_channel_enable()`
// before calling this.
void dacAmpEnable(bool on);

// Optional analog volume set on DACs that expose it (TAS5825M has a 0..255
// digital volume register). On Amped this is a no-op (`AudioEngine` still
// applies software attenuation). Percentages are 0..100.
void dacSetVolume(uint8_t vol_percent);

// Does the DAC have on-chip PEQ we should be pushing coefficients into?
// True on Louder, false on Amped. Read once at boot to pick the EQ backend.
bool dacSupportsHwPeq();

// Push `count` PEQ bands (as high-level user descriptors) to the hardware.
// Called on every `POST /api/eq` after ConfigManager persists the new curve.
//
// Contract:
//   - `count` is in 1..HAL_EQ_MAX_BANDS_ACTIVE. Extra bands are ignored.
//   - Implementation runs `geelen::eqBuildStageList` internally to expand
//     each band into 0-2 biquad stages, converts each stage's float
//     coefficients to Q1.31, and writes them to the DAC.
//   - Left / right channels are ganged for now — per-channel EQ is
//     tracked in `docs/roadmap.md`.
//   - Returns false on any HW error (I2C NAK, page-switch mismatch, over-
//     count). Caller falls back to the software PEQ backend and surfaces
//     `state.eq.backend = "software (fallback)"` for the SPA.
//
// On boards without hardware PEQ this is a no-op that returns false so the
// AudioEngine's software path stays authoritative.
bool dacApplyPeqBands(const geelen::EqBand* bands, uint8_t count);

}  // namespace geelen_hal
