// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Louder-ESP32-Plus with ESP32-WROVER-N8R8 + TAS5825M (I2C-controlled Class-D
// with 15-band biquad DSP on-chip). Pin choices below mirror sonocotta's
// reference Louder-ESP32-Plus schematic (github.com/sonocotta/esp32-tas5805m-dac
// examples). Verify against the actual board revision before flashing.
//
// EQ strategy: `HAL_HAS_HW_PEQ=1` steers the AudioEngine to push all 15 PEQ
// bands into the TAS58xx on-chip biquad slots (Book 0xAA; TAS5825M left
// starts page 0x01 offset 0x30, TAS5805M page 0x24 offset 0x18). Runtime
// DIE_ID selects the map. Software cascades are only used if the HW upload
// fails; in that case the runtime logs a WARN and drops back to
// `HAL_EQ_MAX_BANDS_ACTIVE` bands through the software path.

#pragma once

#include "driver/gpio.h"
#include <cstdint>

#define HAL_BOARD_NAME "Louder-ESP32-Plus (WROVER-N8R8)"

#define HAL_CHIP_FAMILY "esp32"
#define HAL_PSRAM_MB    8

// ---- I2S (audio output) ---------------------------------------------------
// Sonocotta Louder-ESP32-Plus (ESP32): BCK=26, WS=25, DATA=22.
#define HAL_PIN_I2S_BCK  GPIO_NUM_26
#define HAL_PIN_I2S_WS   GPIO_NUM_25
#define HAL_PIN_I2S_DATA GPIO_NUM_22

// ---- Amplifier enable ----------------------------------------------------
// The TAS5825M mutes itself in the DEEP_SLEEP / HI-Z state and unmutes on the
// PLAY transition. PDN (GPIO 33) must be HIGH before I2C; the chip is held
// in HI-Z via registers until `dacAmpEnable` after I2S is stable.
#define HAL_HAS_AMP_EN    1
#define HAL_PIN_AMP_EN    GPIO_NUM_33
#define HAL_I2S_STABLE_MS 50

// ---- DAC control (TAS5825M over I2C) --------------------------------------
// Sonocotta: SDA=21, SCL=27 (not 22 — that is I2S DATA).
#define HAL_HAS_I2C_DAC        1
#define HAL_HAS_HW_PEQ         1
#define HAL_PIN_I2C_SDA        GPIO_NUM_21
#define HAL_PIN_I2C_SCL        GPIO_NUM_27
#define HAL_I2C_FREQ_HZ        400000
#define HAL_TAS5825M_I2C_ADDR  0x4C  // 7-bit address; ADR pin low → 0x4C, high → 0x4D

// Classic ESP32 has no USB PHY; the USB jack is a UART bridge (CH340).
#define HAL_HAS_USB_AUDIO 0

// ---- WS2812B (optional RGB, Sonocotta "RGB OUT" = GPIO 12 on ESP32) ------
#define HAL_HAS_WS2812  1
#define HAL_PIN_WS2812  GPIO_NUM_12
#ifndef HAL_WS2812_COUNT
#define HAL_WS2812_COUNT 8
#endif
#ifndef HAL_WS2812_MAX_DUTY
#define HAL_WS2812_MAX_DUTY 50
#endif

// ---- Rotary encoder (Bourns PEC11R on front panel) -----------------------
// External 10k pull-ups to 3.3V and 0.1µF to GND on A, B, and the switch.
// GPIO 35 is input-only. Do not enable internal pulls. Amp PDN is GPIO 33.
#define HAL_HAS_ENCODER   1
#define HAL_PIN_ENC_A     GPIO_NUM_35
#define HAL_PIN_ENC_B     GPIO_NUM_14
#define HAL_PIN_ENC_SW    GPIO_NUM_13

// ---- EQ cap ---------------------------------------------------------------
// The TAS5825M exposes 15 native biquad slots per channel. We surface all 15
// through the SPA so the user can spend the full HW capacity on shaping.
#define HAL_EQ_MAX_BANDS_ACTIVE 15
