// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Device identity: friendly name, DHCP/mDNS hostname, optional STA/BT MAC
// overrides. Slug/parse helpers are portable; apply/JSON are ESP-only.

#pragma once

#include <cstddef>
#include <cstdint>

#include "idf_config_manager.h"

struct cJSON;

namespace geelen_idf {

// Factory friendly name. mDNS/DHCP hostname stays kFallbackHost when this
// default is in use (slugging "Pareto Amp" would yield pareto-amp).
inline constexpr char kDefaultDeviceName[] = "Pareto Amp";
inline constexpr char kFallbackHost[] = "geelen-audio";

class WifiManager;

// LDH hostname: 1..HOSTNAME_MAX-1 chars, [a-z0-9-], no leading/trailing '-'.
bool identitySlugHostname(const char* device_name, char* out, size_t max_len);
bool identityHostnameValid(const char* hostname);
// Lowercase a copy into `out`. Returns false if the result is not valid LDH.
bool identityNormalizeHostname(const char* in, char* out, size_t max_len);

bool identityParseMac(const char* s, uint8_t out[6]);
void identityFormatMac(const uint8_t mac[6], char out[18]);
// Rejects multicast / all-zero / all-ones. Forces locally-administered bit.
bool identitySanitizeCustomMac(uint8_t mac[6]);
bool identityMacEqual(const uint8_t a[6], const uint8_t b[6]);

#ifdef ESP_PLATFORM

// Snapshot factory MACs, then apply any NVS overrides. Call after
// ConfigManager::begin() and before WifiManager::begin() / A2DP.
void identityInit(ConfigManager* cfg);
void identitySetWifi(WifiManager* wifi);
// Push name/hostname into STA netif, mDNS, and BT GAP (no reboot).
void identityApplyLive();
void identityFactoryMac(int mac_type, uint8_t out[6]);
// `include_macs` false for the SSE state snapshot (name/hostname/mdns only).
void identityAddJson(cJSON* dest, ConfigManager* cfg, bool include_macs);
bool identityCopyHostname(char* out, size_t max_len);
bool identityCopyDeviceName(char* out, size_t max_len);

#endif

}  // namespace geelen_idf
