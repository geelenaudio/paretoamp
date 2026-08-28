// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// STA-only mDNS responder. Advertises `_http._tcp` on port 80 after
// EV_WIFI_UP. Stopped on Wi-Fi down / AP fallback so it never fights
// the captive DNS on the onboarding AP.

#pragma once

namespace geelen_idf {

class ConfigManager;

void mdnsBegin(ConfigManager* cfg);
void mdnsApplyIdentity();
void mdnsStop();
bool mdnsIsUp();

}  // namespace geelen_idf
