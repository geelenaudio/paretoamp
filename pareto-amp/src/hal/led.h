// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// WS2812B strip HAL. Pin, length, and peak duty are compile-time
// (`HAL_PIN_WS2812`, `HAL_WS2812_COUNT`, `HAL_WS2812_MAX_DUTY`). `ledSetRgb`
// fills every pixel the same colour. `ledSetPixel` + `ledRefresh` paint
// a frame (volume bar). Both scale channels by max duty so SPA 100% cannot
// exceed the firmware heat/power cap.

#pragma once

#include <cstdint>

#include "hal/hal_board.h"

namespace geelen_hal {

bool ledPresent();
uint16_t ledPixelCount();
bool ledInit();
void ledSetRgb(uint8_t r, uint8_t g, uint8_t b);
void ledSetPixel(uint16_t i, uint8_t r, uint8_t g, uint8_t b);
void ledRefresh();
void ledClear();

}  // namespace geelen_hal
