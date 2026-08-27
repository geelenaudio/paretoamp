// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Core-0 WS2812 status animator. Slow 0..cap sine: boot red → one green
// breath on STA → BT unpaired blue pulse → user colour/brightness from NVS.

#pragma once

#ifdef ESP_PLATFORM

namespace geelen_idf {

class ConfigManager;

void ledBegin(ConfigManager* cfg);
const char* ledPhaseName();
void ledTakeoverUser();

}  // namespace geelen_idf

#endif
