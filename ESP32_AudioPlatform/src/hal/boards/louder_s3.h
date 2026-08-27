// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Louder-ESP32-S3 with ESP32-S3-WROOM-N8R8 + TAS5825M. S3 defaults; verify
// against the actual Louder-S3 schematic before flashing hardware. See
// `louder_wrover.h` for the EQ / DAC control strategy.

#pragma once

#include "driver/gpio.h"
#include <cstdint>

#define HAL_BOARD_NAME "Louder-ESP32-S3 (WROOM-N8R8)"

#define HAL_CHIP_FAMILY "esp32s3"
#define HAL_PSRAM_MB    8

// ---- I2S (audio output) ---------------------------------------------------
// Placeholders (S3-DevKitC-1 defaults). Confirm on Louder-S3 schematic.
#define HAL_PIN_I2S_BCK  GPIO_NUM_15
#define HAL_PIN_I2S_WS   GPIO_NUM_16
#define HAL_PIN_I2S_DATA GPIO_NUM_7

// ---- Amplifier enable ----------------------------------------------------
#define HAL_HAS_AMP_EN    1
#define HAL_PIN_AMP_EN    GPIO_NUM_17
#define HAL_I2S_STABLE_MS 50

// ---- DAC control (TAS5825M over I2C) --------------------------------------
#define HAL_HAS_I2C_DAC        1
#define HAL_HAS_HW_PEQ         1
#define HAL_PIN_I2C_SDA        GPIO_NUM_8
#define HAL_PIN_I2C_SCL        GPIO_NUM_9
#define HAL_I2C_FREQ_HZ        400000
#define HAL_TAS5825M_I2C_ADDR  0x4C

// ---- WS2812B (optional RGB). GPIO 21 on Louder-Plus S3 (Tindie); do not
// use GPIO 9 — that is I2C SCL on this board.
#define HAL_HAS_WS2812  1
#define HAL_PIN_WS2812  GPIO_NUM_21
#ifndef HAL_WS2812_COUNT
#define HAL_WS2812_COUNT 1
#endif
#ifndef HAL_WS2812_MAX_DUTY
#define HAL_WS2812_MAX_DUTY 50
#endif

// ---- EQ cap ---------------------------------------------------------------
#define HAL_EQ_MAX_BANDS_ACTIVE 15
