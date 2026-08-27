// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Amped-ESP32 (non-plus) with ESP32-WROVER-N8R8 + PCM5100A DAC + TPA3128 amp.
// This is the reference board; every other variant deviates from it.
//
// I2S wiring (fixed by the schematic):
//     BCK  = GPIO 26
//     WS   = GPIO 25
//     DATA = GPIO 22
//     MCLK = unused (PCM5100A is clockless-master; internal PLL)
//
// Amp enable is GPIO 13 driven HIGH after `I2S_STABLE_MS` to avoid the power-on
// pop the TPA3128 emits on its shutdown-to-play transition (see
// `amp_enable_after_i2s_stable` in `idf_main.cpp`).
//
// This board has NO I2C-controlled DAC — the PCM5100A is a fixed-config sink,
// so EQ is done in software (see `AudioEngine::processSamples`). Setting
// HAL_HAS_HW_PEQ=0 makes `geelen_hal::dacApplyPeqBands` return false and the
// AudioEngine falls back to its cascade of biquads.
//
// `HAL_EQ_MAX_BANDS_ACTIVE = 10` — the user-visible band count cap for this
// board. Storage (`geelen::EQ_MAX_BANDS = 15`) stays at the Louder maximum so
// a preset created on Louder round-trips through Amped without losing bands
// (extra bands are truncated on apply, not on read).

#pragma once

#include "driver/gpio.h"
#include <cstdint>

#define HAL_BOARD_NAME "Amped-ESP32 (WROVER-N8R8)"

// Chip / SoC facts, mostly informational — used in `/api/system/identity`.
#define HAL_CHIP_FAMILY "esp32"
#define HAL_PSRAM_MB    8

// ---- I2S (audio output) ---------------------------------------------------
#define HAL_PIN_I2S_BCK  GPIO_NUM_26
#define HAL_PIN_I2S_WS   GPIO_NUM_25
#define HAL_PIN_I2S_DATA GPIO_NUM_22

// ---- Amplifier enable ----------------------------------------------------
#define HAL_HAS_AMP_EN    1
#define HAL_PIN_AMP_EN    GPIO_NUM_13
#define HAL_I2S_STABLE_MS 50

// ---- DAC control (I2C-attached DACs like TAS5825M) ------------------------
// The PCM5100A is not I2C-controlled, so there is no bus and no address.
#define HAL_HAS_I2C_DAC   0
#define HAL_HAS_HW_PEQ    0

// ---- WS2812B (optional RGB, Sonocotta "RGB OUT") -------------------------
// GPIO 12 is MTDI (strapping). Idle-low is safe at reset; init in app_main.
#define HAL_HAS_WS2812  1
#define HAL_PIN_WS2812  GPIO_NUM_12
#ifndef HAL_WS2812_COUNT
#define HAL_WS2812_COUNT 1
#endif
#ifndef HAL_WS2812_MAX_DUTY
#define HAL_WS2812_MAX_DUTY 50
#endif

// ---- EQ cap ---------------------------------------------------------------
// Requested by the product: 10-band parametric EQ on Amped. Storage capacity
// (`EQ_MAX_BANDS`) is higher so a preset saved on Louder round-trips through
// Amped without losing metadata; only the first HAL_EQ_MAX_BANDS_ACTIVE bands
// are pushed to the DSP.
#define HAL_EQ_MAX_BANDS_ACTIVE 10
