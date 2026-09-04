// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Core-0 WS2812 status animator. Slow 0..cap sine: boot red → one green
// breath on STA → BT unpaired blue pulse → user colour/brightness from NVS.

#pragma once

#include <cstdint>

#ifdef ESP_PLATFORM

namespace geelen_idf {

class ConfigManager;

void ledBegin(ConfigManager* cfg);
const char* ledPhaseName();
void ledTakeoverUser();
/** White volume bar for ~2 s, then the current phase (user colour / pairing). */
void ledShowVolume(uint8_t pct, bool muted);

}  // namespace geelen_idf

#endif
