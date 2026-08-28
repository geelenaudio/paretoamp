// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors

#include "idf_identity.h"

#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "idf_mdns.h"
#include "idf_state_stream.h"
#include "idf_wifi_manager.h"
#if CONFIG_BT_ENABLED
#include "idf_a2dp_sink.h"
#endif
#endif

namespace geelen_idf {

static const char* kFallbackHost = "geelen-audio";

bool identityMacEqual(const uint8_t a[6], const uint8_t b[6]) {
  return a && b && memcmp(a, b, 6) == 0;
}

void identityFormatMac(const uint8_t mac[6], char out[18]) {
  if (!out) return;
  if (!mac) {
    out[0] = '\0';
    return;
  }
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

bool identityParseMac(const char* s, uint8_t out[6]) {
  if (!s || !out) return false;
  uint8_t tmp[6] = {};
  int n = 0;
  if (sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%n", &tmp[0], &tmp[1], &tmp[2],
             &tmp[3], &tmp[4], &tmp[5], &n) == 6 &&
      s[n] == '\0') {
    memcpy(out, tmp, 6);
    return true;
  }
  n = 0;
  if (sscanf(s, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%n", &tmp[0], &tmp[1], &tmp[2],
             &tmp[3], &tmp[4], &tmp[5], &n) == 6 &&
      s[n] == '\0') {
    memcpy(out, tmp, 6);
    return true;
  }
  return false;
}

bool identitySanitizeCustomMac(uint8_t mac[6]) {
  if (!mac) return false;
  bool all_zero = true;
  bool all_ones = true;
  for (int i = 0; i < 6; ++i) {
    if (mac[i] != 0x00) all_zero = false;
    if (mac[i] != 0xff) all_ones = false;
  }
  if (all_zero || all_ones) return false;
  if (mac[0] & 0x01) return false;  // multicast
  mac[0] |= 0x02;                   // locally administered
  return true;
}

bool identityHostnameValid(const char* hostname) {
  if (!hostname || !hostname[0]) return false;
  const size_t n = strlen(hostname);
  if (n < 1 || n >= HOSTNAME_MAX) return false;
  if (hostname[0] == '-' || hostname[n - 1] == '-') return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = hostname[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') continue;
    return false;
  }
  return true;
}

bool identityNormalizeHostname(const char* in, char* out, size_t max_len) {
  if (!in || !out || max_len < 2) return false;
  size_t o = 0;
  for (const char* p = in; *p && o + 1 < max_len && o + 1 < HOSTNAME_MAX; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    out[o++] = (char)c;
  }
  out[o] = '\0';
  return identityHostnameValid(out);
}

bool identitySlugHostname(const char* device_name, char* out, size_t max_len) {
  if (!out || max_len < 2) return false;
  const size_t cap = max_len < HOSTNAME_MAX ? max_len : HOSTNAME_MAX;
  size_t o = 0;
  bool last_hyphen = true;  // strip leading hyphens
  if (device_name) {
    for (const char* p = device_name; *p && o + 1 < cap; ++p) {
      unsigned char c = (unsigned char)*p;
      if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
      char w = 0;
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
        w = (char)c;
      } else if (c == ' ' || c == '_' || c == '-' || c == '.') {
        w = '-';
      } else {
        continue;
      }
      if (w == '-' && last_hyphen) continue;
      out[o++] = w;
      last_hyphen = (w == '-');
    }
  }
  while (o > 0 && out[o - 1] == '-') --o;
  out[o] = '\0';
  if (o == 0 || !identityHostnameValid(out)) {
    strncpy(out, kFallbackHost, cap - 1);
    out[cap - 1] = '\0';
  }
  return true;
}

#ifdef ESP_PLATFORM

static const char* TAG = "identity";
static ConfigManager* s_cfg = nullptr;
static WifiManager* s_wifi = nullptr;
static uint8_t s_factory_sta[6] = {};
static uint8_t s_factory_ap[6] = {};
static uint8_t s_factory_bt[6] = {};

static void snapshot_factory_() {
  (void)esp_read_mac(s_factory_sta, ESP_MAC_WIFI_STA);
  (void)esp_read_mac(s_factory_ap, ESP_MAC_WIFI_SOFTAP);
  (void)esp_read_mac(s_factory_bt, ESP_MAC_BT);
}

static void apply_one_(const uint8_t mac[6], esp_mac_type_t type, const char* tag) {
  char buf[18];
  identityFormatMac(mac, buf);
  esp_err_t err = esp_iface_mac_addr_set(mac, type);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_iface_mac_addr_set(%s %s) failed: %s", tag, buf,
             esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "%s MAC override %s", tag, buf);
}

void identityInit(ConfigManager* cfg) {
  s_cfg = cfg;
  snapshot_factory_();
  if (!cfg) return;

  uint8_t sta[6] = {};
  uint8_t bt[6] = {};
  const bool has_sta = cfg->getStaMacOverride(sta);
  const bool has_bt = cfg->getBtMacOverride(bt);
  if (has_sta && has_bt && identityMacEqual(sta, bt)) {
    ESP_LOGE(TAG, "STA and BT MAC overrides collide; leaving factory MACs");
    return;
  }
  if (has_sta) apply_one_(sta, ESP_MAC_WIFI_STA, "STA");
  if (has_bt) apply_one_(bt, ESP_MAC_BT, "BT");
}

void identitySetWifi(WifiManager* wifi) { s_wifi = wifi; }

bool identityCopyHostname(char* out, size_t max_len) {
  if (!s_cfg) return false;
  return s_cfg->getHostname(out, max_len);
}

bool identityCopyDeviceName(char* out, size_t max_len) {
  if (!s_cfg) return false;
  return s_cfg->getDeviceName(out, max_len);
}

void identityFactoryMac(int mac_type, uint8_t out[6]) {
  if (!out) return;
  const uint8_t* src = s_factory_sta;
  if (mac_type == (int)ESP_MAC_WIFI_SOFTAP) src = s_factory_ap;
  else if (mac_type == (int)ESP_MAC_BT) src = s_factory_bt;
  memcpy(out, src, 6);
}

void identityApplyLive() {
  if (!s_cfg) return;
  char host[HOSTNAME_MAX];
  s_cfg->getHostname(host, sizeof(host));
  if (s_wifi) s_wifi->setStaHostname(host);
  mdnsApplyIdentity();
  char name[DEV_NAME_MAX];
  s_cfg->getDeviceName(name, sizeof(name));
#if CONFIG_BT_ENABLED
  a2dpSetAdvertisedName(name[0] ? name : "Geelen Audio");
#endif
  stateStreamMarkDirty();
}

static void add_mac_obj_(cJSON* parent, const char* key, const uint8_t current[6],
                         const uint8_t factory[6], bool overridden) {
  cJSON* o = cJSON_CreateObject();
  char buf[18];
  identityFormatMac(current, buf);
  cJSON_AddStringToObject(o, "current", buf);
  identityFormatMac(factory, buf);
  cJSON_AddStringToObject(o, "factory", buf);
  cJSON_AddBoolToObject(o, "overridden", overridden);
  cJSON_AddItemToObject(parent, key, o);
}

void identityAddJson(cJSON* dest, ConfigManager* cfg, bool include_macs) {
  if (!dest || !cfg) return;
  char name[DEV_NAME_MAX];
  char host[HOSTNAME_MAX];
  cfg->getDeviceName(name, sizeof(name));
  cfg->getHostname(host, sizeof(host));
  cJSON_AddStringToObject(dest, "device_name", name);
  cJSON_AddStringToObject(dest, "hostname", host);
  cJSON_AddBoolToObject(dest, "hostname_custom", cfg->hostnameIsCustom());

  cJSON* mdns = cJSON_CreateObject();
  cJSON_AddStringToObject(mdns, "hostname", host);
  cJSON_AddStringToObject(mdns, "instance", name);
  char url[80];
  snprintf(url, sizeof(url), "http://%s.local/", host);
  cJSON_AddStringToObject(mdns, "url", url);
  cJSON_AddItemToObject(dest, "mdns", mdns);

  if (!include_macs) return;

  uint8_t cur[6] = {};
  cJSON* mac = cJSON_CreateObject();
  if (esp_read_mac(cur, ESP_MAC_WIFI_STA) == ESP_OK) {
    add_mac_obj_(mac, "sta", cur, s_factory_sta, cfg->hasStaMacOverride());
  }
  if (esp_read_mac(cur, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
    add_mac_obj_(mac, "ap", cur, s_factory_ap, cfg->hasStaMacOverride());
  }
  if (esp_read_mac(cur, ESP_MAC_BT) == ESP_OK) {
    add_mac_obj_(mac, "bt", cur, s_factory_bt, cfg->hasBtMacOverride());
  }
  cJSON_AddItemToObject(dest, "mac", mac);
}

#endif  // ESP_PLATFORM

}  // namespace geelen_idf
