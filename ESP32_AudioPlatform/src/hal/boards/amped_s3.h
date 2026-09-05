// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Amped-ESP32-S3 with ESP32-S3-WROOM-N8R8 + PCM5100A DAC + TPA3128 amp.
// The board itself is unchanged from the WROVER Amped except for the SoC.
//
// The S3 has separate strapping / boot / USB pins compared to the WROVER, so
// the audio pin choices are different. Values below are the S3-DevKitC-1
// reference defaults; a production Amped-S3 revision may override them via
// its own header. Do NOT flash this env to real hardware without checking the
// datasheet — the S3 build is BUILD-VALIDATED ONLY in this refactor.
//
// Pin choices rationale (avoiding S3 gotchas):
//   - GPIO 0, 45, 46 are strapping pins → skipped.
//   - GPIO 19 / 20 are USB D-/D+ → skipped.
//   - GPIO 26–32 are the flash/PSRAM lines on -N8R8 → skipped.
//   - GPIO 5–8 are safe general-purpose pins on the S3-DevKitC-1.

#pragma once

#include "driver/gpio.h"
#include <cstdint>

#define HAL_BOARD_NAME "Amped-ESP32-S3 (WROOM-N8R8)"

#define HAL_CHIP_FAMILY "esp32s3"
#define HAL_PSRAM_MB    8

// ---- I2S (audio output) ---------------------------------------------------
// Placeholders (S3-DevKitC-1 defaults). Confirm on the Amped-S3 schematic once
// hardware exists.
#define HAL_PIN_I2S_BCK  GPIO_NUM_5
#define HAL_PIN_I2S_WS   GPIO_NUM_4
#define HAL_PIN_I2S_DATA GPIO_NUM_6

// ---- Amplifier enable ----------------------------------------------------
#define HAL_HAS_AMP_EN    1
#define HAL_PIN_AMP_EN    GPIO_NUM_7
#define HAL_I2S_STABLE_MS 50

// ---- DAC control ---------------------------------------------------------
#define HAL_HAS_I2C_DAC 0
#define HAL_HAS_HW_PEQ  0

// ---- WS2812B (optional RGB, Sonocotta ESP32-S3 "RGB OUT") ----------------
#define HAL_HAS_WS2812  1
#define HAL_PIN_WS2812  GPIO_NUM_9
#ifndef HAL_WS2812_COUNT
#define HAL_WS2812_COUNT 1
#endif
#ifndef HAL_WS2812_MAX_DUTY
#define HAL_WS2812_MAX_DUTY 50
#endif

// ---- EQ cap ---------------------------------------------------------------
// Same 10-band ganged software PEQ as Amped-WROVER. No Unlink (docs/eq.md).
#define HAL_EQ_MAX_BANDS_ACTIVE 10

#define HAL_HAS_ENCODER 0
