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
//   2. Convert each stage's `b0/b1/b2/a1/a2` from `float` to signed Q5.27
//      (1.0 = 0x08000000). TAS5825M DSP range is [-16, 16). Hardware
//      feedback adds a1/a2, so textbook (software) a1/a2 are negated.
//   3. Write left/right slots in Book 0xAA (left starts page 0x01 offset
//      0x30). Unused slots get the identity biquad `{b0=1, b1=b2=a1=a2=0}`.
//   4. Enable 15-band EQ via Book 0x8C page 0x0B (gang from channel mode,
//      bypass off). TAS5805M uses DSP_MISC; independent Book 0xAA RAM.
//      Disabling EQ writes bypass on so analog returns to dry.
//
// If ANY step returns a non-OK code, this function returns `false` and the
// caller falls back to the software PEQ path (see `on_eq_changed` in
// `idf_main.cpp`).

#include "hal/dac.h"

#if HAL_HAS_I2C_DAC

#include "hal/hal_board.h"
#include "audio_engine.h"
#include "eq_coeffs.h"
#include "eq_q131.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
bool s_play_ok = false;

#if HAL_HAS_TAS5805M_DRIVER
static void tas_status_later(void*) {
  vTaskDelay(pdMS_TO_TICKS(2500));
  if (s_hw_ready) tas5805m_log_status();
  vTaskDelete(nullptr);
}
#endif

}  // namespace

#if HAL_HAS_TAS5805M_DRIVER
static bool try_chip_init() {
  if (s_hw_ready) return true;
  if (!s_i2c_bus) return false;
  tas5805m_config_t cfg{};
  cfg.i2c_bus = s_i2c_bus;
  cfg.i2c_addr = HAL_TAS5825M_I2C_ADDR;
  if (tas5805m_init(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "tas5805m_init failed at 0x%02x", (unsigned)HAL_TAS5825M_I2C_ADDR);
    return false;
  }
  s_hw_ready = true;
  ESP_LOGI(TAG, "TAS58xx initialised at 0x%02x model=%s",
           (unsigned)HAL_TAS5825M_I2C_ADDR,
           tas5805m_probed_model() == TAS5805M_MODEL_5805 ? "TAS5805M" : "TAS5825M");
  return true;
}
#endif

bool dacInit() {
  ESP_LOGI(TAG, "DAC (%s) init — hardware PEQ backend", HAL_BOARD_NAME);

  // PDN must be HIGH before I2C: shutdown kills the slave, and IDF 5.5
  // reports a stuck/NACK transaction as ESP_ERR_INVALID_STATE.
#if HAL_HAS_AMP_EN
  gpio_reset_pin(HAL_PIN_AMP_EN);
  gpio_config_t io{};
  io.intr_type = GPIO_INTR_DISABLE;
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << HAL_PIN_AMP_EN;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io);
  gpio_set_level(HAL_PIN_AMP_EN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
#endif

  gpio_reset_pin(HAL_PIN_I2C_SDA);
  gpio_reset_pin(HAL_PIN_I2C_SCL);

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

  ESP_LOGI(TAG, "I2C bus up SDA=%d SCL=%d (chip init waits for I2S clocks)",
           (int)HAL_PIN_I2C_SDA, (int)HAL_PIN_I2C_SCL);
  return true;
}

void dacAmpEnable(bool on) {
#if HAL_HAS_TAS5805M_DRIVER
  if (on) {
#if HAL_HAS_AMP_EN
    // Full analog reset: a latched DC/OC fault survives I2C PLAY.
    gpio_set_level(HAL_PIN_AMP_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(HAL_PIN_AMP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_hw_ready = false;
    s_play_ok = false;
#endif
    try_chip_init();
    if (s_hw_ready) {
      if (tas5805m_enter_play() != ESP_OK) {
        ESP_LOGE(TAG, "tas5805m_enter_play failed");
      } else {
        s_play_ok = true;
        xTaskCreate(tas_status_later, "tas_stat", 3072, nullptr, 1, nullptr);
      }
    }
  } else if (s_hw_ready) {
    tas5805m_set_state(TAS5805M_CTRL_HIZ);
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
  if (s_play_ok) {
    tas5805m_set_volume_pct(vol_percent);
  }
#endif
  (void)vol_percent;
}

bool dacHasHwVolume() {
#if HAL_HAS_TAS5805M_DRIVER
  return s_play_ok;
#else
  return false;
#endif
}

void dacLogStatus() {
#if HAL_HAS_TAS5805M_DRIVER
  if (s_hw_ready) tas5805m_log_status();
#endif
}

bool dacSupportsHwPeq() {
#if HAL_HAS_TAS5805M_DRIVER
  return s_play_ok;
#else
  return false;
#endif
}

bool dacApplyPeqBandsLR(const geelen::EqBand* left, uint8_t nL,
                        const geelen::EqBand* right, uint8_t nR, bool ganged) {
#if HAL_HAS_TAS5805M_DRIVER
  if (!s_play_ok || !left || nL == 0) return false;
  if (!ganged && (!right || nR == 0)) return false;

  geelen::EqStage stagesL[geelen::EQ_MAX_STAGES_PER_LAYER];
  geelen::EqStage stagesR[geelen::EQ_MAX_STAGES_PER_LAYER];
  const size_t nStagesL =
      geelen::eqBuildStageList((float)geelen::SAMPLE_RATE, left, nL, stagesL);
  const geelen::EqStage* rightStages = stagesL;
  size_t nStagesR = nStagesL;
  if (!ganged) {
    nStagesR = geelen::eqBuildStageList((float)geelen::SAMPLE_RATE, right, nR, stagesR);
    rightStages = stagesR;
  }
  if (nStagesL > 15 || nStagesR > 15) {
    ESP_LOGW(TAG, "PEQ expansion L=%u R=%u stages, using first 15 TAS slots",
             (unsigned)nStagesL, (unsigned)nStagesR);
  }

  tas5805m_set_eq_ganged(ganged);
  if (tas5805m_set_eq_enabled(false) != ESP_OK) return false;

  auto fail_bypass = []() {
    (void)tas5805m_set_eq_enabled(false);
    return false;
  };

  auto write_bank = [&](const geelen::EqStage* stages, size_t n, TAS5805M_EQ_CHANNELS ch) {
    const size_t slots = (n > 15) ? 15 : n;
    for (uint8_t i = 0; i < 15; ++i) {
      int32_t hw[5];
      if (i < slots) {
        geelen::eqStageToTas5825mQ5_27(stages[i].b0, stages[i].b1, stages[i].b2,
                                       stages[i].a1, stages[i].a2, hw);
      } else {
        hw[0] = 0x08000000;
        hw[1] = hw[2] = hw[3] = hw[4] = 0;
      }
      if (tas5805m_set_bq_coeffs(i, ch, hw[0], hw[1], hw[2], hw[3], hw[4]) != ESP_OK) {
        return false;
      }
    }
    return true;
  };

  if (!write_bank(stagesL, nStagesL, TAS5805M_CH_LEFT)) return fail_bypass();
  if (!write_bank(rightStages, nStagesR, TAS5805M_CH_RIGHT)) return fail_bypass();

  if (tas5805m_set_eq_enabled(true) != ESP_OK) return false;
  ESP_LOGI(TAG, "HW PEQ uploaded (L=%u R=%u stages, %s)",
           (unsigned)((nStagesL > 15) ? 15 : nStagesL),
           (unsigned)((nStagesR > 15) ? 15 : nStagesR),
           ganged ? "ganged" : "split");
  return true;
#else
  (void)left;
  (void)nL;
  (void)right;
  (void)nR;
  (void)ganged;
  return false;
#endif
}

bool dacApplyPeqBands(const geelen::EqBand* bands, uint8_t count) {
  return dacApplyPeqBandsLR(bands, count, bands, count, true);
}

bool dacSetHwPeqEnabled(bool on) {
#if HAL_HAS_TAS5805M_DRIVER
  if (!s_play_ok) return false;
  if (tas5805m_set_eq_enabled(on) != ESP_OK) return false;
  ESP_LOGI(TAG, "HW PEQ %s", on ? "enabled" : "bypassed");
  return true;
#else
  (void)on;
  return false;
#endif
}

}  // namespace geelen_hal

#endif  // HAL_HAS_I2C_DAC
