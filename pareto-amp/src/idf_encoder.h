// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Rotary encoder (volume / mute / pair) on boards with HAL_HAS_ENCODER.
// No-op on Amped: GPIO 13 is amp-enable there.

#pragma once

#ifdef ESP_PLATFORM

namespace geelen_idf {

class ConfigManager;

void encoderBegin(ConfigManager* cfg);

}  // namespace geelen_idf

#endif
