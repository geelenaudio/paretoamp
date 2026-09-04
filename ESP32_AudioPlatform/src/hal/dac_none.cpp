// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// No-op DAC HAL for the Amped boards (PCM5100A). The DAC has no I2C control
// path; volume and EQ are entirely software (see AudioEngine::processSamples).
// The only real work here is toggling the amp-enable GPIO — every other
// entry point is a stub that documents the "software everywhere" contract.

#include "hal/dac.h"

#if !HAL_HAS_I2C_DAC

#include "hal/hal_board.h"
#include "driver/gpio.h"
#include "esp_log.h"

namespace geelen_hal {

static const char* TAG = "DAC/none";

bool dacInit() {
#if HAL_HAS_AMP_EN
  gpio_config_t io{};
  io.intr_type = GPIO_INTR_DISABLE;
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = 1ULL << HAL_PIN_AMP_EN;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io);
  // Start with the amp muted; caller flips it on after I2S clocks stabilise.
  gpio_set_level(HAL_PIN_AMP_EN, 0);
#endif
  ESP_LOGI(TAG, "DAC (%s) init — software PEQ backend", HAL_BOARD_NAME);
  return true;
}

void dacAmpEnable(bool on) {
#if HAL_HAS_AMP_EN
  gpio_set_level(HAL_PIN_AMP_EN, on ? 1 : 0);
  ESP_LOGI(TAG, "Amp enable (GPIO %d) %s", (int)HAL_PIN_AMP_EN, on ? "HIGH" : "LOW");
#else
  (void)on;
#endif
}

void dacSetVolume(uint8_t /*vol_percent*/) {
  // No-op: AudioEngine already applies volume in software.
}

bool dacHasHwVolume() { return false; }

void dacLogStatus() {}

bool dacSupportsHwPeq() { return false; }

bool dacApplyPeqBands(const geelen::EqBand* /*bands*/, uint8_t /*count*/) {
  // Amped has no hardware PEQ. Returning false steers `on_eq_changed` into
  // its software-cascade branch so the AudioEngine picks up the new bands.
  return false;
}

bool dacApplyPeqBandsLR(const geelen::EqBand* /*left*/, uint8_t /*nL*/,
                        const geelen::EqBand* /*right*/, uint8_t /*nR*/, bool /*ganged*/) {
  return false;
}

bool dacSetHwPeqEnabled(bool /*on*/) { return false; }

}  // namespace geelen_hal

#endif  // !HAL_HAS_I2C_DAC
