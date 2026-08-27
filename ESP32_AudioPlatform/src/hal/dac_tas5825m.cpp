// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// TAS5825M DAC HAL for the Louder boards. Wraps the vendored sonocotta driver
// (`components/tas5805m/`, GPL-3.0-or-later, see COPYING) with:
//
//   - I2C bus bring-up on the pins declared in `hal/boards/louder_*.h`.
//   - Amp state-machine transitions (HI-Z → PLAY on `dacAmpEnable(true)`).
//   - Volume passthrough (0..100 → 0..255 register).
//   - Raw biquad-coefficient upload for on-chip 15-band PEQ.
//
// Coefficient upload details (see `components/tas5805m/README.md` and TAS5825M
// datasheet Chapter 8):
//
//   1. Expand each user band into 0-2 `EqStage`s using `eqBuildStageList`.
//   2. Convert each stage's `b0/b1/b2/a1/a2` from `float` to signed Q1.31.
//      Values are clamped to [-1, 1) — the biquads emitted by `eq_coeffs.cpp`
//      never exceed that range for the frequencies / gains we accept.
//   3. Write left-channel band `n` to Book 0x8C, page 0x2C + n. Right channel
//      is page 0x38 + n. Each band is 20 bytes: 5 int32 coefficients, big-endian,
//      packed contiguously.
//   4. Unused bands (up to 15) are zeroed with the identity biquad
//      `{b0=1.0, b1=b2=a1=a2=0}` so the DSP passes signal through.
//   5. Commit the page bank via the driver's `commit_eq_page` helper — which
//      does the `0x7F=0x00 (page reset), 0x00=0x00, 0x7F=0x00, ...` dance the
//      datasheet requires between EQ updates.
//
// If ANY step returns a non-OK code, this function returns `false` and the
// caller falls back to the software PEQ path (see `on_eq_changed` in
// `idf_main.cpp`).

#include "hal/dac.h"

#if HAL_HAS_I2C_DAC

#include "hal/hal_board.h"
#include "eq_coeffs.h"
#include "eq_q131.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

#include <cmath>
#include <cstring>

// The sonocotta driver headers are only present when the tas5805m component
// is enabled (Louder builds; see components/tas5805m/CMakeLists.txt gate).
// Feature-detect via a preprocessor probe so this .cpp can be compiled on
// Amped builds too (with all functions turning into stubs that return false).
#if __has_include("tas5805m.h")
#  include "tas5805m.h"
#  define HAL_HAS_TAS5805M_DRIVER 1
#else
#  define HAL_HAS_TAS5805M_DRIVER 0
#endif

namespace geelen_hal {

static const char* TAG = "DAC/TAS5825M";

namespace {

i2c_master_bus_handle_t s_i2c_bus = nullptr;
bool s_hw_ready = false;

// Convert a floating-point biquad coefficient to signed Q1.31.
// Range is [-1.0, +1.0); we clamp so a rogue value (e.g. NaN from a bad
// user input we somehow missed) never wraps to a huge negative int.
int32_t floatToQ1_31(float v) {
  return geelen::eqFloatToQ1_31(v);
}

// Convert one EqStage into 5 Q1.31 coefficients in the DAC's expected order:
// b0, b1, b2, a1, a2. First-order (one-pole) stages get zero'd b2/a2 so they
// still fit the biquad slot format.
void stageToQ1_31(const geelen::EqStage& st, int32_t out[5]) {
  out[0] = floatToQ1_31(st.b0);
  out[1] = floatToQ1_31(st.b1);
  out[2] = floatToQ1_31(st.b2);
  out[3] = floatToQ1_31(st.a1);
  out[4] = floatToQ1_31(st.a2);
}

// Identity biquad: passthrough with unity gain. Q1.31 for 1.0 is INT32_MAX
// (the maximum representable positive value in Q1.31 saturates at ~ +1.0).
void identityBiquadQ1_31(int32_t out[5]) {
  out[0] = INT32_MAX;
  out[1] = 0;
  out[2] = 0;
  out[3] = 0;
  out[4] = 0;
}

}  // namespace

bool dacInit() {
  ESP_LOGI(TAG, "DAC (%s) init — hardware PEQ backend", HAL_BOARD_NAME);

  // PDN line: keep the DAC in shutdown while we bring up I2C.
#if HAL_HAS_AMP_EN
  gpio_config_t io{};
  io.intr_type = GPIO_INTR_DISABLE;
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << HAL_PIN_AMP_EN;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io);
  gpio_set_level(HAL_PIN_AMP_EN, 0);
#endif

  i2c_master_bus_config_t bus_cfg{};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = HAL_PIN_I2C_SDA;
  bus_cfg.scl_io_num = HAL_PIN_I2C_SCL;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &s_i2c_bus) != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed on SDA=%d SCL=%d",
             (int)HAL_PIN_I2C_SDA, (int)HAL_PIN_I2C_SCL);
    return false;
  }

#if HAL_HAS_TAS5805M_DRIVER
  // Sonocotta driver bring-up. Details of the actual call signature are
  // whatever the vendored component exposes; see components/tas5805m/README.md
  // for the shape we require.
  tas5805m_config_t cfg{};
  cfg.i2c_bus = s_i2c_bus;
  cfg.i2c_addr = HAL_TAS5825M_I2C_ADDR;
  if (tas5805m_init(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "tas5805m_init failed at 0x%02x", (unsigned)HAL_TAS5825M_I2C_ADDR);
    return false;
  }
  s_hw_ready = true;
  ESP_LOGI(TAG, "TAS5825M initialised at 0x%02x", (unsigned)HAL_TAS5825M_I2C_ADDR);
  return true;
#else
  ESP_LOGW(TAG,
           "TAS5825M driver header not found. This Louder build was compiled "
           "without the vendored sonocotta component; add "
           "`components/tas5805m/` to your idf_component.yml or drop the "
           "sonocotta source into `components/tas5805m/`. Falling back to "
           "software PEQ.");
  s_hw_ready = false;
  return false;
#endif
}

void dacAmpEnable(bool on) {
#if HAL_HAS_TAS5805M_DRIVER
  if (s_hw_ready) {
    // HI-Z → PLAY on enable, PLAY → HI-Z on disable. sonocotta exposes the
    // full 4-state machine but we only use these two transitions.
    tas5805m_set_state(on ? TAS5805M_CTRL_PLAY : TAS5805M_CTRL_HIZ);
  }
#endif
#if HAL_HAS_AMP_EN
  gpio_set_level(HAL_PIN_AMP_EN, on ? 1 : 0);
#endif
  ESP_LOGI(TAG, "Amp enable %s", on ? "HIGH (PLAY)" : "LOW (HI-Z)");
}

void dacSetVolume(uint8_t vol_percent) {
  if (vol_percent > 100) vol_percent = 100;
#if HAL_HAS_TAS5805M_DRIVER
  if (s_hw_ready) {
    tas5805m_set_volume_pct(vol_percent);
  }
#endif
  (void)vol_percent;
}

bool dacSupportsHwPeq() {
#if HAL_HAS_TAS5805M_DRIVER
  return s_hw_ready;
#else
  return false;
#endif
}

bool dacApplyPeqBands(const geelen::EqBand* bands, uint8_t count) {
#if !HAL_HAS_TAS5805M_DRIVER
  (void)bands; (void)count;
  return false;
#else
  if (!s_hw_ready) return false;
  if (!bands || count == 0) return false;

  // TAS5825M has exactly 15 BQ slots per channel. If the stage-expansion
  // exceeds 15 (a 24 dB/oct shelf produces 2 stages, so a user who fills
  // all 15 bands with 24 dB/oct filters would hit 30) we prioritise the
  // first-N-stages and log a WARN so the SPA can surface a "truncated"
  // notice on the next `/api/state` snapshot.
  constexpr uint8_t kMaxHwBands = 15;
  if (count > kMaxHwBands) count = kMaxHwBands;

  geelen::EqStage stages[geelen::EQ_MAX_STAGES_PER_LAYER];
  const size_t stage_count = geelen::eqBuildStageList(44100.0f, bands, count, stages);
  if (stage_count > kMaxHwBands) {
    ESP_LOGW(TAG,
             "EQ expansion produced %u stages > %u HW bands. Truncating; "
             "consider reducing 24 dB/oct filters or lowering the band count.",
             (unsigned)stage_count, (unsigned)kMaxHwBands);
  }
  const uint8_t used = (uint8_t)((stage_count > kMaxHwBands) ? kMaxHwBands : stage_count);

  for (uint8_t band = 0; band < kMaxHwBands; ++band) {
    int32_t coeffs[5];
    if (band < used) {
      stageToQ1_31(stages[band], coeffs);
    } else {
      identityBiquadQ1_31(coeffs);
    }
    // Left AND right (ganged). Per-channel EQ is on the Phase-10 roadmap.
    if (tas5805m_set_bq_coeffs(band, TAS5805M_CH_LEFT,
                               coeffs[0], coeffs[1], coeffs[2],
                               coeffs[3], coeffs[4]) != ESP_OK) {
      ESP_LOGE(TAG, "BQ upload L band %u failed", (unsigned)band);
      return false;
    }
    if (tas5805m_set_bq_coeffs(band, TAS5805M_CH_RIGHT,
                               coeffs[0], coeffs[1], coeffs[2],
                               coeffs[3], coeffs[4]) != ESP_OK) {
      ESP_LOGE(TAG, "BQ upload R band %u failed", (unsigned)band);
      return false;
    }
  }
  if (tas5805m_commit_eq_page() != ESP_OK) {
    ESP_LOGE(TAG, "tas5805m_commit_eq_page failed");
    return false;
  }
  ESP_LOGI(TAG, "PEQ upload OK: %u user bands → %u HW slots", (unsigned)count, (unsigned)used);
  return true;
#endif
}

}  // namespace geelen_hal

#endif  // HAL_HAS_I2C_DAC
